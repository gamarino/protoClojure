#include "Reader.h"

#include "runtime/ListBuilder.h"
#include "runtime/MapOps.h"
#include "runtime/StackGuard.h"
#include "runtime/VectorOps.h"

#include "protoCore.h"

#include <vector>

namespace protoClojure {

Reader::Reader(proto::ProtoContext* ctx, std::string source,
               const NamedLayout& named, SourceInfo* info)
    : ctx_(ctx), lexer_(std::move(source)), named_(named), info_(info) {}

namespace {

// How a map-literal key is spelled in the "Duplicate key" read error: the
// source text of a keyword, a symbol, a string or an integer, and a generic
// description of anything else (the reader has no printer of its own).
std::string describeKey(proto::ProtoContext* ctx, const NamedLayout& named,
                        const proto::ProtoObject* key) {
    if (key == PROTO_NONE) return "nil";
    if (key == PROTO_TRUE) return "true";
    if (key == PROTO_FALSE) return "false";
    if (isNamed(ctx, named, key)) return namedSpelling(ctx, named, key)->toStdString(ctx);
    if (proto::ProtoObject::isStringTagFast(key))
        return "\"" + reinterpret_cast<const proto::ProtoString*>(key)->toStdString(ctx) + "\"";
    if (key->isInteger(ctx)) return key->asIntegerString(ctx, 10)->toStdString(ctx);
    return "a repeated form";
}

} // namespace

// Forms nested deeper than the thread's stack allows raise StackOverflowError
// in readFromToken; readOne and readAll report it as a read error at the
// position where reading stopped, so every driver handles it like any other
// read error.
const proto::ProtoObject* Reader::readOne() {
    try {
        Token tok = lexer_.next();
        if (tok.kind == TokenKind::EndOfFile) return nullptr;
        return readFromToken(ctx_, tok);
    } catch (const StackOverflowError& e) {
        throw ReaderError(e.what(), lexer_.line(), lexer_.column());
    }
}

const proto::ProtoList* Reader::readAll() {
    try {
        return readAllForms();
    } catch (const StackOverflowError& e) {
        throw ReaderError(e.what(), lexer_.line(), lexer_.column());
    }
}

const proto::ProtoList* Reader::readAllForms() {
    // The accumulator IS a ProtoList the protoCore GC traces — no std
    // container in sight — and it is grown in chunks, so the intermediate
    // versions a long file produces are reclaimed while the file is still
    // being read instead of being pinned until the last form is parsed.
    // Forms are read through the builder's context: a thread's contexts form
    // a LIFO stack and the builder must stay its innermost frame while it is
    // open (see ListBuilder.h).
    ListBuilder forms(ctx_);

    while (true) {
        Token tok = lexer_.next();
        if (tok.kind == TokenKind::EndOfFile) break;
        forms.push(readFromToken(forms.context(), tok));
    }
    return forms.finishList();
}

const proto::ProtoObject*
Reader::readFromToken(proto::ProtoContext* parent, const Token& tok) {
    // Every nested form recurses through here (readList, `@`).
    checkNativeStack(StackUse::Source);
    switch (tok.kind) {
        case TokenKind::Integer:
            // Atomic — no intermediate state to protect. The caller takes
            // ownership of rooting the return value. A literal beyond the
            // long long range is built from its digits as an exact
            // LargeInteger (protoCore parses under its own critical section).
            if (!tok.fitsLong) return parent->fromString(tok.text.c_str(), 10);
            return parent->fromLong(tok.intValue);

        case TokenKind::Float:
            return parent->fromDouble(tok.doubleValue);

        case TokenKind::String:
            // A string literal is the protoCore string a program builds at
            // run time. Symbols are named values (runtime/Named.h), never
            // strings, so nothing is lost when protoCore stores a short
            // string inline in the pointer.
            return parent->fromUTF8String(tok.text.c_str());

        case TokenKind::Symbol: {
            // `nil`, `true` and `false` are the values themselves, as in
            // Clojure's reader. Every other symbol and every keyword (the
            // lexer spells a keyword with its leading `:`) is the interned
            // named value of its spelling: one pointer per spelling for the
            // life of the process, the same value `(quote foo)` and `:foo`
            // evaluate to.
            if (tok.text == "nil")   return PROTO_NONE;
            if (tok.text == "true")  return PROTO_TRUE;
            if (tok.text == "false") return PROTO_FALSE;
            return internNamed(parent, named_, tok.text.c_str());
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
            const proto::ProtoObject* derefSym =
                internNamed(&wrapScope, named_, "deref");
            const proto::ProtoObject* lst =
                wrapScope.newList()->asObject(&wrapScope);
            lst = lst->asList(&wrapScope)
                ->appendLast(&wrapScope, derefSym)
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
            // `[..]` is a vector, the value `(vector ..)` builds at run time
            // (runtime/VectorOps.h): the items are read into a ProtoList and
            // boxed, an O(1) step.
            proto::ProtoContext wrapScope(parent->space, parent);
            wrapScope.resizeAutomaticLocals(1);
            wrapScope.setAutomaticLocal(0,
                readList(&wrapScope, TokenKind::RBracket, tok.line, tok.column));
            return newVector(&wrapScope,
                wrapScope.getAutomaticLocal(0)->asList(&wrapScope));
        }

        case TokenKind::LBrace: {
            // `{k1 v1 k2 v2 ...}` is a map, the value `(hash-map ..)` builds
            // at run time (runtime/MapOps.h). The forms are read in source
            // order into a ProtoList, then associated. As in Clojure, an odd
            // number of forms and a repeated key are read errors.
            proto::ProtoContext wrapScope(parent->space, parent);
            wrapScope.resizeAutomaticLocals(1);
            wrapScope.setAutomaticLocal(0,
                readList(&wrapScope, TokenKind::RBrace, tok.line, tok.column));
            const proto::ProtoList* lst =
                wrapScope.getAutomaticLocal(0)->asList(&wrapScope);
            const proto::proto_ulong n = lst->getSize(&wrapScope);
            if (n % 2 != 0) {
                throw ReaderError("Map literal must contain an even number of forms",
                                  tok.line, tok.column);
            }
            // The forms stay reachable through the rooted list while the map
            // is built; protoCore does not move objects.
            std::vector<const proto::ProtoObject*> kv;
            kv.reserve(n);
            for (proto::proto_ulong i = 0; i < n; ++i)
                kv.push_back(lst->getAt(&wrapScope, static_cast<int>(i)));
            const proto::ProtoObject* m =
                mapAssocPairs(&wrapScope, nullptr, kv.data(), n);
            if (mapCount(&wrapScope, m) != n / 2) {
                for (proto::proto_ulong i = 2; i < n; i += 2) {
                    for (proto::proto_ulong j = 0; j < i; j += 2) {
                        if (keyEquals(&wrapScope, kv[j], kv[i])) {
                            throw ReaderError(
                                "Duplicate key: " + describeKey(&wrapScope, named_, kv[i]),
                                tok.line, tok.column);
                        }
                    }
                }
            }
            if (info_ && n > 0) {
                std::vector<const proto::ProtoObject*>& keys = info_->mapKeyOrder[m];
                keys.clear();
                for (proto::proto_ulong i = 0; i < n; i += 2) keys.push_back(kv[i]);
            }
            return m;
        }

        case TokenKind::Error:
            throw ReaderError(tok.text, tok.line, tok.column);

        case TokenKind::RParen:
            throw ReaderError("unexpected )", tok.line, tok.column);

        case TokenKind::EndOfFile:
            throw ReaderError("unexpected end of file", tok.line, tok.column);

        default:
            throw ReaderError(
                std::string("unsupported token: ") +
                tokenKindName(tok.kind),
                tok.line, tok.column);
    }
}

const proto::ProtoObject*
Reader::readList(proto::ProtoContext* parent, TokenKind closeKind,
                 int openLine, int openColumn) {
    // One read scope per recursive call, as a ListBuilder: it owns a child
    // context of `parent` that roots the in-progress list, and it folds the
    // elements in in chunks so a large literal — `[0 1 … 69999]` — does not
    // pin every intermediate version of the list until the form is closed.
    //
    // The builder's context is the innermost one on this thread while it is
    // open, so nested forms are read through `items.context()`; a context
    // chained to `parent` instead would unlink the builder from the thread's
    // root chain when it destructs (see ListBuilder.h).
    //
    // The returned ProtoObject* is not rooted by us. The caller's contract is
    // to put it in its OWN slot immediately — the protoCore calling
    // convention: returns are unrooted, the caller takes responsibility.
    ListBuilder items(parent);

    while (true) {
        Token tok = lexer_.next();
        if (tok.kind == TokenKind::EndOfFile) {
            const char* what = "unterminated list — missing `)`";
            if (closeKind == TokenKind::RBracket) what = "unterminated vector — missing `]`";
            else if (closeKind == TokenKind::RBrace) what = "unterminated map — missing `}`";
            throw ReaderError(what, openLine, openColumn);
        }
        if (tok.kind == closeKind) {
            return items.finish();
        }
        if (tok.kind == TokenKind::RParen ||
            tok.kind == TokenKind::RBracket ||
            tok.kind == TokenKind::RBrace) {
            throw ReaderError(
                std::string("mismatched ") + tokenKindName(tok.kind),
                tok.line, tok.column);
        }

        // `push` parks the form in a GC-visible slot as its FIRST action,
        // before anything allocates, so the element cannot be reclaimed
        // between the read that produced it and the append that consumes it.
        items.push(readFromToken(items.context(), tok));
    }
}

} // namespace protoClojure
