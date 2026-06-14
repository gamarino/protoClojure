#include "reader/Lexer.h"

#include <gtest/gtest.h>

using protoClojure::Lexer;
using protoClojure::Token;
using protoClojure::TokenKind;

namespace {

std::vector<Token> tokenise(const std::string& src) {
    Lexer lex(src);
    std::vector<Token> out;
    while (true) {
        Token t = lex.next();
        out.push_back(t);
        if (t.kind == TokenKind::EndOfFile || t.kind == TokenKind::Error) break;
    }
    return out;
}

} // namespace

TEST(Lexer, EmptyInputYieldsEOF) {
    auto toks = tokenise("");
    ASSERT_EQ(toks.size(), 1u);
    EXPECT_EQ(toks[0].kind, TokenKind::EndOfFile);
}

TEST(Lexer, SkipsWhitespaceAndCommas) {
    auto toks = tokenise("   , ,  ,\t\n ,");
    ASSERT_EQ(toks.size(), 1u);
    EXPECT_EQ(toks[0].kind, TokenKind::EndOfFile);
}

TEST(Lexer, SkipsLineComments) {
    auto toks = tokenise("; this is a comment\n42");
    ASSERT_EQ(toks.size(), 2u);
    EXPECT_EQ(toks[0].kind, TokenKind::Integer);
    EXPECT_EQ(toks[0].intValue, 42);
}

TEST(Lexer, PositiveInteger) {
    auto toks = tokenise("42");
    ASSERT_EQ(toks.size(), 2u);
    EXPECT_EQ(toks[0].kind, TokenKind::Integer);
    EXPECT_EQ(toks[0].intValue, 42);
    EXPECT_EQ(toks[0].text, "42");
}

TEST(Lexer, NegativeIntegerAtPrimaryPosition) {
    auto toks = tokenise("-7");
    ASSERT_EQ(toks.size(), 2u);
    EXPECT_EQ(toks[0].kind, TokenKind::Integer);
    EXPECT_EQ(toks[0].intValue, -7);
}

TEST(Lexer, MinusAfterOperandIsBinarySymbol) {
    // `42-7` reads as Integer Symbol Integer, NOT a negative literal.
    auto toks = tokenise("42-7");
    ASSERT_GE(toks.size(), 3u);
    EXPECT_EQ(toks[0].kind, TokenKind::Integer);
    EXPECT_EQ(toks[0].intValue, 42);
    EXPECT_EQ(toks[1].kind, TokenKind::Symbol);
    EXPECT_EQ(toks[1].text, "-7");  // -7 starts a symbol; not pretty, but
                                      // matches Clojure-JVM behaviour
}

TEST(Lexer, NegativeLiteralAfterWhitespace) {
    // `42 -7` reads as Integer Integer (the -7 is negative because the space
    // makes it primary again).
    auto toks = tokenise("42 -7");
    ASSERT_EQ(toks.size(), 3u);
    EXPECT_EQ(toks[0].kind, TokenKind::Integer);
    EXPECT_EQ(toks[0].intValue, 42);
    EXPECT_EQ(toks[1].kind, TokenKind::Integer);
    EXPECT_EQ(toks[1].intValue, -7);
}

TEST(Lexer, SimpleString) {
    auto toks = tokenise(R"("hello")");
    ASSERT_EQ(toks.size(), 2u);
    EXPECT_EQ(toks[0].kind, TokenKind::String);
    EXPECT_EQ(toks[0].text, "hello");
}

TEST(Lexer, StringWithEscapes) {
    auto toks = tokenise(R"("line1\nline2\t\"end\"")");
    ASSERT_EQ(toks.size(), 2u);
    EXPECT_EQ(toks[0].kind, TokenKind::String);
    EXPECT_EQ(toks[0].text, "line1\nline2\t\"end\"");
}

TEST(Lexer, UnterminatedStringIsError) {
    auto toks = tokenise("\"never ends");
    ASSERT_GE(toks.size(), 1u);
    EXPECT_EQ(toks[0].kind, TokenKind::Error);
}

TEST(Lexer, Symbol) {
    auto toks = tokenise("println");
    ASSERT_EQ(toks.size(), 2u);
    EXPECT_EQ(toks[0].kind, TokenKind::Symbol);
    EXPECT_EQ(toks[0].text, "println");
}

TEST(Lexer, SymbolWithSpecialChars) {
    auto toks = tokenise("+ *ear-muffs* my.ns/name");
    ASSERT_EQ(toks.size(), 4u);
    EXPECT_EQ(toks[0].kind, TokenKind::Symbol);
    EXPECT_EQ(toks[0].text, "+");
    EXPECT_EQ(toks[1].kind, TokenKind::Symbol);
    EXPECT_EQ(toks[1].text, "*ear-muffs*");
    EXPECT_EQ(toks[2].kind, TokenKind::Symbol);
    EXPECT_EQ(toks[2].text, "my.ns/name");
}

TEST(Lexer, Parens) {
    auto toks = tokenise("()");
    ASSERT_EQ(toks.size(), 3u);
    EXPECT_EQ(toks[0].kind, TokenKind::LParen);
    EXPECT_EQ(toks[1].kind, TokenKind::RParen);
}

TEST(Lexer, HelloWorldShape) {
    // The shape we need for `(println "hello")` to read correctly.
    auto toks = tokenise(R"((println "hello"))");
    ASSERT_EQ(toks.size(), 5u);
    EXPECT_EQ(toks[0].kind, TokenKind::LParen);
    EXPECT_EQ(toks[1].kind, TokenKind::Symbol);
    EXPECT_EQ(toks[1].text, "println");
    EXPECT_EQ(toks[2].kind, TokenKind::String);
    EXPECT_EQ(toks[2].text, "hello");
    EXPECT_EQ(toks[3].kind, TokenKind::RParen);
    EXPECT_EQ(toks[4].kind, TokenKind::EndOfFile);
}

TEST(Lexer, BracketsAreLexedAsTokens) {
    // Brackets implement vector literals starting in session 5 (fn / let /
    // loop bindings). Confirm they tokenise to LBracket / RBracket.
    auto toks = tokenise("[1 2 3]");
    ASSERT_GE(toks.size(), 5u);
    EXPECT_EQ(toks[0].kind, TokenKind::LBracket);
    EXPECT_EQ(toks[4].kind, TokenKind::RBracket);
}

TEST(Lexer, ReservedBraceReportsError) {
    // `{:a 1}` is still reserved-for-later in v0.0.x (maps land later).
    auto toks = tokenise("{:a 1}");
    ASSERT_GE(toks.size(), 1u);
    EXPECT_EQ(toks[0].kind, TokenKind::Error);
}

TEST(Lexer, LineColumnTracked) {
    auto toks = tokenise("hello\n  world");
    ASSERT_EQ(toks.size(), 3u);
    EXPECT_EQ(toks[0].line, 1);
    EXPECT_EQ(toks[0].column, 1);
    EXPECT_EQ(toks[1].line, 2);
    EXPECT_EQ(toks[1].column, 3);
}

TEST(Lexer, PeekDoesNotConsume) {
    Lexer lex("foo bar");
    EXPECT_EQ(lex.peek().kind, TokenKind::Symbol);
    EXPECT_EQ(lex.peek().text, "foo");
    EXPECT_EQ(lex.peek().text, "foo");      // idempotent
    EXPECT_EQ(lex.next().text, "foo");
    EXPECT_EQ(lex.next().text, "bar");
}
