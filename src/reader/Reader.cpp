#include "Reader.h"

#include "protoCore.h"

namespace protoClojure {

Reader::Reader(proto::ProtoContext* ctx, std::string source)
    : ctx_(ctx), lexer_(std::move(source)) {}

const proto::ProtoObject* Reader::readOne() {
    Token tok = lexer_.next();
    if (tok.kind == TokenKind::EndOfFile) return nullptr;
    return readFromToken(tok);
}

std::vector<const proto::ProtoObject*> Reader::readAll() {
    std::vector<const proto::ProtoObject*> out;
    while (true) {
        Token tok = lexer_.next();
        if (tok.kind == TokenKind::EndOfFile) break;
        out.push_back(readFromToken(tok));
    }
    return out;
}

const proto::ProtoObject* Reader::readFromToken(const Token& tok) {
    switch (tok.kind) {
        case TokenKind::Integer:
            return ctx_->fromLong(tok.intValue);

        case TokenKind::String:
            // fromUTF8String — NOT interned. Each string literal allocates a
            // fresh ProtoString. Identity is meaningless for strings.
            return ctx_->fromUTF8String(tok.text.c_str());

        case TokenKind::Symbol: {
            // createSymbol — interned for the lifetime of the ProtoSpace.
            // Two reads of `foo` in the same session return THE SAME pointer.
            // That identity is what the evaluator uses to look up vars.
            //
            // Cast through ProtoObject for storage — the symbol IS a
            // ProtoString, but the AST holds ProtoObject*.
            return reinterpret_cast<const proto::ProtoObject*>(
                proto::ProtoString::createSymbol(ctx_, tok.text.c_str()));
        }

        case TokenKind::LParen:
            return readList(tok.line, tok.column);

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

const proto::ProtoObject* Reader::readList(int openLine, int openColumn) {
    const proto::ProtoList* lst = ctx_->newList();
    while (true) {
        Token tok = lexer_.next();
        if (tok.kind == TokenKind::EndOfFile) {
            throw ReaderError("unterminated list — missing `)`",
                              openLine, openColumn);
        }
        if (tok.kind == TokenKind::RParen) {
            // appendLast returns a new ProtoList each call; the final value
            // is what we return.
            return lst->asObject(ctx_);
        }
        const proto::ProtoObject* elem = readFromToken(tok);
        lst = lst->appendLast(ctx_, elem);
    }
}

} // namespace protoClojure
