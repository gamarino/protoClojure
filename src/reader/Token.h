/*
 * Token — the lexer's output element.
 *
 * Session 2 scope: just enough to tokenise `(println "hello")` and simple
 * arithmetic. Keywords, booleans (true/false/nil), brackets/braces, quote and
 * the reader-macro prefixes (`#`, `` ` ``, `~`, `~@`, `^`) land as later
 * sessions need them. The enum carries placeholders for the full v0.1 set so
 * subsequent additions only add real handling, not enum entries.
 */
#pragma once
#include <cstdint>
#include <string>

namespace protoClojure {

enum class TokenKind : uint8_t {
    Integer,
    Float,           // reserved — not yet emitted
    Ratio,           // reserved
    String,
    Char,            // reserved (\a, \space, etc.)
    Symbol,          // foo, +, *ear-muffs*, my.ns/name
    Keyword,         // reserved — `:foo`
    Nil,             // reserved — `nil`
    True,            // reserved — `true`
    False,           // reserved — `false`

    LParen,          // (
    RParen,          // )
    LBracket,        // reserved — [
    RBracket,        // reserved — ]
    LBrace,          // reserved — {
    RBrace,          // reserved — }
    HashLBrace,      // reserved — #{
    HashLParen,      // reserved — #(

    Quote,           // reserved — '
    Backtick,        // reserved — `
    Tilde,           // reserved — ~
    TildeAt,         // reserved — ~@
    HashUnderscore,  // reserved — #_
    HashApostrophe,  // reserved — #'
    Caret,           // reserved — ^
    At,              // @  (session 16 — reader macro for `(deref ...)`)

    EndOfFile,
    Error,           // text carries the error message
};

struct Token {
    TokenKind   kind;
    std::string text;           // raw lexeme (or error message when kind=Error)
    long long   intValue = 0;   // valid for Integer
    double      doubleValue = 0.0;  // valid for Float (session 9)
    int         line   = 1;
    int         column = 1;
};

const char* tokenKindName(TokenKind k);

} // namespace protoClojure
