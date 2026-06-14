#include "Reader.h"

#include "protoCore.h"

namespace protoClojure {

Reader::Reader(proto::ProtoContext* ctx, std::string source,
               const ReaderMarkers& markers)
    : ctx_(ctx), lexer_(std::move(source)), markers_(markers) {}

const proto::ProtoObject* Reader::readOne() {
    Token tok = lexer_.next();
    if (tok.kind == TokenKind::EndOfFile) return nullptr;
    return readFromToken(ctx_, tok);
}

const proto::ProtoList* Reader::readAll() {
    // One child context for the whole readAll session. Two slots: the
    // accumulator ProtoList (slot 0) and the most recently read form
    // (slot 1). Same shape as readList — the accumulator IS a ProtoList,
    // the protoCore GC traces it, no std container in sight.
    proto::ProtoContext scope(ctx_->space, ctx_);
    scope.resizeAutomaticLocals(2);
    constexpr unsigned int kSlotAcc  = 0;
    constexpr unsigned int kSlotForm = 1;
    scope.setAutomaticLocal(kSlotAcc,
                            scope.newList()->asObject(&scope));

    while (true) {
        Token tok = lexer_.next();
        if (tok.kind == TokenKind::EndOfFile) break;
        scope.setAutomaticLocal(kSlotForm,
                                readFromToken(&scope, tok));
        const proto::ProtoList* cur =
            scope.getAutomaticLocal(kSlotAcc)->asList(&scope);
        const proto::ProtoList* updated =
            cur->appendLast(&scope, scope.getAutomaticLocal(kSlotForm));
        scope.setAutomaticLocal(kSlotAcc, updated->asObject(&scope));
    }
    return scope.getAutomaticLocal(kSlotAcc)->asList(&scope);
}

const proto::ProtoObject*
Reader::readFromToken(proto::ProtoContext* parent, const Token& tok) {
    switch (tok.kind) {
        case TokenKind::Integer:
            // Atomic — no intermediate state to protect. The caller takes
            // ownership of rooting the return value.
            return parent->fromLong(tok.intValue);

        case TokenKind::Float:
            return parent->fromDouble(tok.doubleValue);

        case TokenKind::String: {
            // String literal — wrap so the Compiler can distinguish it from
            // a same-bytes inline symbol (protoCore inlines short bytes into
            // the pointer and loses the symbol/string distinction at that
            // representation; see ReaderMarkers doc). The raw ProtoString
            // is stashed under bytesKey on a fresh mutable child of
            // stringMarkerProto.
            const proto::ProtoObject* raw =
                parent->fromUTF8String(tok.text.c_str());
            const proto::ProtoObject* wrap =
                markers_.stringMarkerProto->newChild(parent,
                                                      /*isMutable=*/true);
            const_cast<proto::ProtoObject*>(wrap)
                ->setAttribute(parent, markers_.bytesKey, raw);
            return wrap;
        }

        case TokenKind::Symbol: {
            // createSymbol — interned for the lifetime of the ProtoSpace.
            // Two reads of `foo` in the same session return THE SAME pointer.
            // That identity is what the evaluator will use to look up vars.
            return reinterpret_cast<const proto::ProtoObject*>(
                proto::ProtoString::createSymbol(parent, tok.text.c_str()));
        }

        case TokenKind::LParen:
            return readList(parent, TokenKind::RParen,
                            tok.line, tok.column);

        case TokenKind::At: {
            // Session 16 — `@expr` desugars to `(deref expr)` at read
            // time. Build a fresh list with two elements: the symbol
            // `deref` and the next form read. The list is rooted
            // immediately into a child scope so an allocation in the
            // recursive readOne cannot reclaim it before it's wired
            // into a parent slot by the caller.
            proto::ProtoContext wrapScope(parent->space, parent);
            wrapScope.resizeAutomaticLocals(2);
            constexpr unsigned int kSlotList = 0;
            constexpr unsigned int kSlotForm = 1;
            // (deref ...)
            const proto::ProtoString* derefSym =
                proto::ProtoString::createSymbol(&wrapScope, "deref");
            const proto::ProtoObject* lst =
                wrapScope.newList()->asObject(&wrapScope);
            lst = lst->asList(&wrapScope)
                ->appendLast(&wrapScope,
                    reinterpret_cast<const proto::ProtoObject*>(derefSym))
                ->asObject(&wrapScope);
            wrapScope.setAutomaticLocal(kSlotList, lst);
            // Read the next form (could be a symbol, list, vector, ...).
            Token next = lexer_.next();
            if (next.kind == TokenKind::EndOfFile) {
                throw ReaderError("`@` must be followed by a form",
                                  tok.line, tok.column);
            }
            wrapScope.setAutomaticLocal(kSlotForm,
                readFromToken(&wrapScope, next));
            lst = wrapScope.getAutomaticLocal(kSlotList);
            lst = lst->asList(&wrapScope)
                ->appendLast(&wrapScope,
                    wrapScope.getAutomaticLocal(kSlotForm))
                ->asObject(&wrapScope);
            return lst;
        }

        case TokenKind::LBracket: {
            // Session 9 — `[..]` is now a tagged vector literal, distinct
            // from `(..)` lists. We still parse the items into a ProtoList
            // (same shape the rest of the reader produces), but wrap it
            // under vectorMarkerProto with the items list under itemsKey
            // so the compiler can tell `[x y]` apart from `(x y)`. The
            // compiler unwraps for fn/let/loop bindings; for expressions
            // it emits a `(vector x y)` call so a ProtoTuple materialises
            // at runtime.
            proto::ProtoContext wrapScope(parent->space, parent);
            wrapScope.resizeAutomaticLocals(2);
            constexpr unsigned int kSlotItems = 0;
            constexpr unsigned int kSlotWrap  = 1;
            const proto::ProtoObject* items =
                readList(&wrapScope, TokenKind::RBracket,
                         tok.line, tok.column);
            wrapScope.setAutomaticLocal(kSlotItems, items);
            wrapScope.setAutomaticLocal(kSlotWrap,
                markers_.vectorMarkerProto->newChild(&wrapScope,
                                                      /*isMutable=*/true));
            const_cast<proto::ProtoObject*>(
                wrapScope.getAutomaticLocal(kSlotWrap))
                ->setAttribute(&wrapScope, markers_.itemsKey,
                               wrapScope.getAutomaticLocal(kSlotItems));
            return wrapScope.getAutomaticLocal(kSlotWrap);
        }

        case TokenKind::LBrace: {
            // Session 13 — `{k1 v1 k2 v2 ...}` map literal. Same wrapping
            // pattern as vectors but under mapMarkerProto. The entries
            // list stores k,v,k,v,... in source order; the compiler
            // checks the even-count rule (odd → reader error). The
            // compiler emits `(hash-map k1 v1 ...)` for the expression
            // case and unwraps directly for fn `& {:keys ...}`
            // destructuring.
            proto::ProtoContext wrapScope(parent->space, parent);
            wrapScope.resizeAutomaticLocals(2);
            constexpr unsigned int kSlotEntries = 0;
            constexpr unsigned int kSlotWrap    = 1;
            const proto::ProtoObject* entries =
                readList(&wrapScope, TokenKind::RBrace,
                         tok.line, tok.column);
            wrapScope.setAutomaticLocal(kSlotEntries, entries);
            const proto::ProtoList* lst =
                wrapScope.getAutomaticLocal(kSlotEntries)->asList(&wrapScope);
            if (lst->getSize(&wrapScope) % 2 != 0) {
                throw ReaderError("map literal: odd number of forms",
                                  tok.line, tok.column);
            }
            wrapScope.setAutomaticLocal(kSlotWrap,
                markers_.mapMarkerProto->newChild(&wrapScope,
                                                   /*isMutable=*/true));
            const_cast<proto::ProtoObject*>(
                wrapScope.getAutomaticLocal(kSlotWrap))
                ->setAttribute(&wrapScope, markers_.entriesKey,
                               wrapScope.getAutomaticLocal(kSlotEntries));
            return wrapScope.getAutomaticLocal(kSlotWrap);
        }

        case TokenKind::Error:
            throw ReaderError(tok.text, tok.line, tok.column);

        case TokenKind::RParen:
            throw ReaderError("unexpected )", tok.line, tok.column);

        case TokenKind::EndOfFile:
            throw ReaderError("unexpected end of file", tok.line, tok.column);

        default:
            throw ReaderError(
                std::string("not yet implemented in v0.0.x: token kind ") +
                tokenKindName(tok.kind),
                tok.line, tok.column);
    }
}

const proto::ProtoObject*
Reader::readList(proto::ProtoContext* parent, TokenKind closeKind,
                 int openLine, int openColumn) {
    // One ProtoContext per recursive read scope. Child of `parent`, with two
    // slots: the in-progress list (slot 0) and the most recent element
    // (slot 1). Both are GC-visible until this context destructs.
    //
    // When destructs, the returned ProtoObject* is no longer rooted by us.
    // The caller's contract is to put it in its OWN slot immediately. This is
    // the protoCore calling convention — returns are unrooted, the caller
    // takes responsibility.
    proto::ProtoContext frame(parent->space, parent);
    frame.resizeAutomaticLocals(2);
    constexpr unsigned int kSlotList = 0;
    constexpr unsigned int kSlotElem = 1;

    frame.setAutomaticLocal(kSlotList,
                            frame.newList()->asObject(&frame));

    while (true) {
        Token tok = lexer_.next();
        if (tok.kind == TokenKind::EndOfFile) {
            const char* what = "unterminated list — missing `)`";
            if (closeKind == TokenKind::RBracket) what = "unterminated vector — missing `]`";
            else if (closeKind == TokenKind::RBrace) what = "unterminated map — missing `}`";
            throw ReaderError(what, openLine, openColumn);
        }
        if (tok.kind == closeKind) {
            return frame.getAutomaticLocal(kSlotList);
        }
        if (tok.kind == TokenKind::RParen ||
            tok.kind == TokenKind::RBracket ||
            tok.kind == TokenKind::RBrace) {
            throw ReaderError(
                std::string("mismatched ") + tokenKindName(tok.kind),
                tok.line, tok.column);
        }

        // Read the next element into a slot IMMEDIATELY after the call
        // returns it, so an allocation in appendLast below cannot reclaim
        // it via concurrent mark.
        frame.setAutomaticLocal(kSlotElem,
                                readFromToken(&frame, tok));

        // Append: read both pointers from slots, build the new list, store
        // back into the list slot. The intermediate `cur` and `updated`
        // exist only for the next statement, never across an allocation.
        const proto::ProtoList* cur =
            frame.getAutomaticLocal(kSlotList)->asList(&frame);
        const proto::ProtoList* updated =
            cur->appendLast(&frame, frame.getAutomaticLocal(kSlotElem));
        frame.setAutomaticLocal(kSlotList, updated->asObject(&frame));
    }
}

} // namespace protoClojure
