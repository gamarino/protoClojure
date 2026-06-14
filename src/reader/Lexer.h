/*
 * Lexer — turns source text into a stream of Tokens.
 *
 * Hand-written single-character lookahead. The same shape protoST's Lexer
 * uses, adapted for Clojure conventions (commas as whitespace, `;` line
 * comments, the broader symbol alphabet).
 *
 * Session 2 scope: integers (incl. negative literals at primary position),
 * double-quoted strings with backslash escapes, symbols, and parentheses.
 * Everything else in TokenKind is reserved and will be implemented as later
 * sessions need it.
 */
#pragma once
#include "Token.h"
#include <string>

namespace protoClojure {

class Lexer {
public:
    explicit Lexer(std::string source);

    // Pull the next token. Drives the stream forward.
    Token next();

    // Look at the next token without consuming it. Multiple peek() calls in a
    // row return the same token; the next next() returns it.
    const Token& peek();

private:
    std::string source_;
    std::size_t pos_  = 0;
    int         line_ = 1;
    int         column_ = 1;

    bool        hasPeek_ = false;
    Token       peekTok_;
    TokenKind   prevReturnedKind_ = TokenKind::EndOfFile;

    Token nextImpl_();

    bool   eof() const { return pos_ >= source_.size(); }
    char   current() const { return source_[pos_]; }
    char   lookahead() const {
        return (pos_ + 1 < source_.size()) ? source_[pos_ + 1] : '\0';
    }
    void   advance();
    void   skipWhitespaceAndComments();

    Token lexNumber(bool negative);
    Token lexString();
    Token lexSymbolOrPunct();

    // A `-` is the start of a negative literal only when the previous token
    // does not end an operand (matches the D1 rule from protoST's lexer).
    bool prevEndsOperand() const;

    Token makeError(const std::string& msg, int errLine, int errCol);
};

} // namespace protoClojure
