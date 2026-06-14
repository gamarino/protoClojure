#include "Reader.h"

#include "protoCore.h"

namespace protoClojure {

Reader::Reader(proto::ProtoContext* ctx, std::string source)
    : ctx_(ctx), lexer_(std::move(source)) {}

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

        case TokenKind::String:
            // fromUTF8String — NOT interned. Each string literal allocates a
            // fresh ProtoString. Identity is meaningless for strings.
            return parent->fromUTF8String(tok.text.c_str());

        case TokenKind::Symbol: {
            // createSymbol — interned for the lifetime of the ProtoSpace.
            // Two reads of `foo` in the same session return THE SAME pointer.
            // That identity is what the evaluator will use to look up vars.
            return reinterpret_cast<const proto::ProtoObject*>(
                proto::ProtoString::createSymbol(parent, tok.text.c_str()));
        }

        case TokenKind::LParen:
            return readList(parent, tok.line, tok.column);

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
Reader::readList(proto::ProtoContext* parent, int openLine, int openColumn) {
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
            throw ReaderError("unterminated list — missing `)`",
                              openLine, openColumn);
        }
        if (tok.kind == TokenKind::RParen) {
            // Snapshot the value, then let `frame` destruct on the natural
            // return. The caller is responsible for rooting.
            return frame.getAutomaticLocal(kSlotList);
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
