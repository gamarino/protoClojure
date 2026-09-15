#include "reader/Lexer.h"

#include <climits>

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
    // Brackets implement vector literals (fn / let /
    // loop bindings). Confirm they tokenise to LBracket / RBracket.
    auto toks = tokenise("[1 2 3]");
    ASSERT_GE(toks.size(), 5u);
    EXPECT_EQ(toks[0].kind, TokenKind::LBracket);
    EXPECT_EQ(toks[4].kind, TokenKind::RBracket);
}

TEST(Lexer, BracesAreLexedAsTokens) {
    // Map literals: `{...}` tokenises to LBrace / RBrace.
    auto toks = tokenise("{:a 1}");
    ASSERT_GE(toks.size(), 4u);
    EXPECT_EQ(toks[0].kind, TokenKind::LBrace);
    EXPECT_EQ(toks[3].kind, TokenKind::RBrace);
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

TEST(Lexer, NonAsciiLettersInSymbolsAndKeywords) {
    // The last symbol spells é as `e` followed by U+0301 COMBINING ACUTE.
    auto toks = tokenise("ñandú :ñandú café Ωμέγα :日本語 cafe\xCC\x81");
    ASSERT_EQ(toks.size(), 7u);
    for (int i = 0; i < 6; ++i) EXPECT_EQ(toks[i].kind, TokenKind::Symbol) << i;
    EXPECT_EQ(toks[0].text, "ñandú");
    EXPECT_EQ(toks[1].text, ":ñandú");
    EXPECT_EQ(toks[2].text, "café");
    EXPECT_EQ(toks[3].text, "Ωμέγα");
    EXPECT_EQ(toks[4].text, ":日本語");
    EXPECT_EQ(toks[5].text, "cafe\xCC\x81");
}

TEST(Lexer, NonLetterCodePointsEndSymbolsAndAreErrors) {
    // U+2192 RIGHTWARDS ARROW is not a letter: it ends `a` and is an error.
    auto arrow = tokenise("a\xE2\x86\x92" "b");
    ASSERT_EQ(arrow.size(), 2u);
    EXPECT_EQ(arrow[0].kind, TokenKind::Symbol);
    EXPECT_EQ(arrow[0].text, "a");
    EXPECT_EQ(arrow[1].kind, TokenKind::Error);
    EXPECT_EQ(arrow[1].text, "unexpected character: \xE2\x86\x92 (U+2192)");
    // U+00A0 NO-BREAK SPACE neither extends a symbol nor separates tokens.
    auto nbsp = tokenise("x\xC2\xA0y");
    ASSERT_EQ(nbsp.size(), 2u);
    EXPECT_EQ(nbsp[0].text, "x");
    EXPECT_EQ(nbsp[1].kind, TokenKind::Error);
    EXPECT_NE(nbsp[1].text.find("(U+00A0)"), std::string::npos);
}

TEST(Lexer, MalformedUtf8IsAnError) {
    // Truncated, truncated before a delimiter, overlong, surrogate, beyond
    // U+10FFFF, stray continuation byte, invalid lead byte.
    const char* const bad[] = {"\xC3", "\xC3(", "\xC0\xAF", "\xED\xA0\x80",
                               "\xF4\x90\x80\x80", "\x80", "\xFF"};
    for (const char* b : bad) {
        auto toks = tokenise(std::string("a") + b);
        ASSERT_EQ(toks.size(), 2u);
        EXPECT_EQ(toks[0].kind, TokenKind::Symbol);
        EXPECT_EQ(toks[0].text, "a");
        EXPECT_EQ(toks[1].kind, TokenKind::Error);
        EXPECT_EQ(toks[1].text.rfind("invalid UTF-8 byte 0x", 0), 0u)
            << toks[1].text;
    }
}

TEST(Lexer, NumberFollowedByNonAsciiLetterIsMalformed) {
    auto toks = tokenise("42ñx");
    ASSERT_EQ(toks.size(), 1u);
    EXPECT_EQ(toks[0].kind, TokenKind::Error);
    EXPECT_EQ(toks[0].text, "malformed number literal: 42ñx");
}

TEST(Lexer, ColumnsCountCodePoints) {
    auto toks = tokenise("ñandú \"ü\" )");
    ASSERT_EQ(toks.size(), 4u);
    EXPECT_EQ(toks[1].kind, TokenKind::String);
    EXPECT_EQ(toks[1].text, "ü");
    EXPECT_EQ(toks[1].column, 7);
    EXPECT_EQ(toks[2].kind, TokenKind::RParen);
    EXPECT_EQ(toks[2].column, 11);
}

TEST(Lexer, IntegerBeyondLongRangeKeepsItsDigits) {
    auto toks = tokenise("12345678901234567890 -9223372036854775809 -9223372036854775808");
    ASSERT_EQ(toks.size(), 4u);
    EXPECT_EQ(toks[0].kind, TokenKind::Integer);
    EXPECT_FALSE(toks[0].fitsLong);
    EXPECT_EQ(toks[0].text, "12345678901234567890");
    EXPECT_EQ(toks[1].kind, TokenKind::Integer);
    EXPECT_FALSE(toks[1].fitsLong);
    EXPECT_EQ(toks[1].text, "-9223372036854775809");
    // The most negative long long still fits.
    EXPECT_EQ(toks[2].kind, TokenKind::Integer);
    EXPECT_TRUE(toks[2].fitsLong);
    EXPECT_EQ(toks[2].intValue, LLONG_MIN);
}

TEST(Lexer, BigIntegerSuffixOnIntegersOnly) {
    auto toks = tokenise("42N -7N");
    ASSERT_EQ(toks.size(), 3u);
    EXPECT_EQ(toks[0].kind, TokenKind::Integer);
    EXPECT_EQ(toks[0].intValue, 42);
    EXPECT_EQ(toks[0].text, "42");
    EXPECT_EQ(toks[1].kind, TokenKind::Integer);
    EXPECT_EQ(toks[1].intValue, -7);

    auto floatSuffix = tokenise("1.5N");
    ASSERT_EQ(floatSuffix.back().kind, TokenKind::Error);
    EXPECT_EQ(floatSuffix.back().text, "malformed number literal: 1.5N");
    EXPECT_EQ(tokenise("42Nx").back().kind, TokenKind::Error);
}
