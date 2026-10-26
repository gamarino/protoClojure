// Unit tests for the regular-expression layer (src/runtime/Regex.h): the
// translation of Java's dialect to PCRE2's, the compiled-pattern cache,
// matching from an offset, Java's walk over successive matches, the
// engine's limits, and pattern values.

#include "runtime/Regex.h"
#include "runtime/StackGuard.h"

#include "protoCore.h"
#include <gtest/gtest.h>

#include <string>

using protoClojure::compilePattern;
using protoClojure::Match;
using protoClojure::PatternSyntaxError;

namespace {

std::string firstMatch(const char* pattern, const std::string& subject) {
    Match m;
    if (!protoClojure::regexSearch(compilePattern(pattern), subject, 0, m)) return "<none>";
    return m.str(subject);
}

std::string wholeMatch(const char* pattern, const std::string& subject) {
    Match m;
    if (!protoClojure::regexMatchWhole(compilePattern(pattern), subject, m)) return "<none>";
    return m.str(subject);
}

std::string allMatches(const char* pattern, const std::string& s) {
    std::string out;
    protoClojure::forEachMatch(s, compilePattern(pattern), [&](const Match& m) {
        out += "[" + m.str(s) + "]";
        return true;
    });
    return out;
}

std::string translated(const char* pattern) {
    unsigned options = 0;
    bool unixLines = false;
    return protoClojure::translatePattern(pattern, options, unixLines);
}

} // namespace

TEST(Regex, CacheReturnsOneCompiledPatternPerSource) {
    const auto* a = compilePattern("a+b");
    EXPECT_EQ(a, compilePattern("a+b"));
    EXPECT_NE(a, compilePattern("a+c"));
    EXPECT_EQ(a->source, "a+b");
    EXPECT_EQ(compilePattern("(a)(?:b)(c)")->groups, 2u);
    EXPECT_EQ(compilePattern("(?<word>\\w+)(?<=d)")->groups, 1u);
}

TEST(Regex, JitIsUsedWhereTheLibraryHasOne) {
    // Every CI platform's PCRE2 has a JIT; a build without one still
    // matches, through the interpreter.
    EXPECT_TRUE(compilePattern("a+b")->jit);
}

TEST(Regex, JavaInlineFlagsAnywhere) {
    EXPECT_EQ(firstMatch("(?i)ABC", "xabcx"), "abc");
    EXPECT_EQ(firstMatch("(?s)a.b", "a\nb"), "a\nb");
    EXPECT_EQ(firstMatch("a.b", "a\nb"), "<none>");
    EXPECT_EQ(firstMatch("(?is)A.B", "a\nb"), "a\nb");
    EXPECT_EQ(firstMatch("a(?i)B", "aB ab Ab"), "aB");
    EXPECT_EQ(firstMatch("a(?i:B)c", "abC abc"), "abc");
    EXPECT_EQ(firstMatch("(?i)a(?-i)B", "Ab AB"), "AB");
    EXPECT_EQ(firstMatch("(?u)(?iu)\xC3\x89", "\xC3\xA9"), "\xC3\xA9");  // (?u) accepted
    // Java's COMMENTS mode ignores white space in classes too (PCRE2's xx).
    EXPECT_EQ(firstMatch("(?x) a b # comment\n c", "abc"), "abc");
    EXPECT_EQ(firstMatch("(?x)[a b]+", "x ab"), "ab");
    // UNIX_LINES: only LF ends a line.
    EXPECT_EQ(firstMatch("(?d)a.b", "a\rb"), "a\rb");
    EXPECT_EQ(firstMatch("a.b", "a\rb"), "<none>");
}

TEST(Regex, JavaConstructsStdRegexLacked) {
    EXPECT_EQ(firstMatch("(?<=\\$)\\d+", "cost $42"), "42");
    EXPECT_EQ(firstMatch("(?<!x)y", "xy ay"), "y");
    EXPECT_EQ(firstMatch("a++b", "aaab"), "aaab");
    EXPECT_EQ(firstMatch("a++a", "aaaa"), "<none>");             // possessive
    EXPECT_EQ(firstMatch("(?>a+)b", "aab"), "aab");               // atomic
    EXPECT_EQ(firstMatch("(?<d>\\d)\\k<d>", "12 33"), "33");      // named back-reference
    EXPECT_EQ(firstMatch("\\p{Lu}+", "abc\xC3\x89T\xC3\xA9"), "\xC3\x89T");
    EXPECT_EQ(firstMatch("\\p{IsAlphabetic}+", "1\xC3\xA9t\xC3\xA9!"), "\xC3\xA9t\xC3\xA9");
    EXPECT_EQ(firstMatch("\\p{Punct}+", "ab!?c"), "!?");
    EXPECT_EQ(firstMatch("[\\p{Digit}x]+", "ab1x2c"), "1x2");
    EXPECT_EQ(firstMatch("\\P{Alpha}+", "ab12c"), "12");
    EXPECT_EQ(firstMatch("\\u00e9+", "x\xC3\xA9\xC3\xA9y"), "\xC3\xA9\xC3\xA9");
    EXPECT_EQ(firstMatch("\\uD83D\\uDE00", "a\xF0\x9F\x98\x80"), "\xF0\x9F\x98\x80");
    EXPECT_EQ(firstMatch("\\0101", "zA"), "A");                   // Java octal
    EXPECT_EQ(firstMatch("[a-c[x-z]]+", "mbyq"), "by");           // union
    EXPECT_EQ(firstMatch("[[:a]]+", "b:a:c"), ":a:");             // Java: a nested class
    EXPECT_EQ(firstMatch("\\Q*.[\\E", "a*.[b"), "*.[");
    EXPECT_EQ(firstMatch("[\\Q]\\E]", "a]b"), "]");
    EXPECT_EQ(firstMatch("[]a]+", "x]a]"), "]a]");               // a leading ] is a member
    EXPECT_EQ(firstMatch("\\Aab\\z", "ab"), "ab");
    EXPECT_EQ(firstMatch("ab\\Z", "ab\n"), "ab");
    EXPECT_EQ(firstMatch("ab$", "ab\n"), "ab");                   // before a final terminator
    EXPECT_EQ(firstMatch("ab$", "ab\r\n"), "ab");
    EXPECT_EQ(firstMatch("[.]", "a.b"), ".");
}

TEST(Regex, CharactersAreCodePoints) {
    EXPECT_EQ(firstMatch("^.$", "\xC3\xA9"), "\xC3\xA9");
    EXPECT_EQ(firstMatch("[\xC3\xA9\xE2\x82\xAC]+", "a\xE2\x82\xAC\xC3\xA9z"), "\xE2\x82\xAC\xC3\xA9");
    EXPECT_EQ(allMatches("", "a\xC3\xA9"), "[][][]");            // one empty match per character
    // \w and \d are ASCII, as in Java without UNICODE_CHARACTER_CLASS ...
    EXPECT_EQ(firstMatch("\\w+", "\xC3\xA9t\xC3\xA9"), "t");
    // ... and Unicode with a leading (?U).
    EXPECT_EQ(firstMatch("(?U)\\w+", "\xC3\xA9t\xC3\xA9"), "\xC3\xA9t\xC3\xA9");
    // A subject that is not valid UTF-8 matches around its bad bytes.
    EXPECT_EQ(firstMatch("b+", std::string("a\xFF\xFE" "bb")), "bb");
    EXPECT_EQ(firstMatch("a.b", std::string("a\xFF" "b")), "<none>");
}

TEST(Regex, UnsupportedSyntaxIsAPatternSyntaxError) {
    EXPECT_THROW(compilePattern("(ab"), PatternSyntaxError);
    EXPECT_THROW(compilePattern("a{2,1}"), PatternSyntaxError);
    EXPECT_THROW(compilePattern("[a-z&&[^b]]"), PatternSyntaxError);
    EXPECT_THROW(compilePattern("[a[^b]]"), PatternSyntaxError);
    EXPECT_THROW(compilePattern("\\N{LATIN SMALL LETTER A}"), PatternSyntaxError);
    EXPECT_THROW(compilePattern("a(?d)b"), PatternSyntaxError);
    EXPECT_THROW(compilePattern("a(?U)b"), PatternSyntaxError);
    EXPECT_THROW(compilePattern("(?q)a"), PatternSyntaxError);
    EXPECT_THROW(compilePattern("\\u12"), PatternSyntaxError);
    EXPECT_THROW(compilePattern("\\p{NoSuchProperty}"), PatternSyntaxError);
    try {
        compilePattern("x(ab");
        FAIL() << "expected PatternSyntaxError";
    } catch (const PatternSyntaxError& e) {
        EXPECT_NE(std::string(e.what()).find("x(ab"), std::string::npos) << e.what();
    }
}

TEST(Regex, TranslationIsMinimal) {
    EXPECT_EQ(translated("(?<n>a)\\k<n>(?<=b)c++"), "(?<n>a)\\k<n>(?<=b)c++");
    EXPECT_EQ(translated("(?iu)a(?u)b"), "(?i)ab");
    EXPECT_EQ(translated("(?x)a"), "(?xx)a");
    EXPECT_EQ(translated("\\u0041\\p{Alpha}[\\p{Alpha}]"), "\\x{41}[[:alpha:]][[:alpha:]]");
}

TEST(Regex, WholeMatchIsJavasMatches) {
    EXPECT_EQ(wholeMatch("\\d+", "123"), "123");
    EXPECT_EQ(wholeMatch("\\d+", "123a"), "<none>");
    EXPECT_EQ(wholeMatch("a|ab", "ab"), "ab");  // not the leftmost-first "a"
    EXPECT_EQ(wholeMatch("ab$", "ab\n"), "<none>");
}

// A search from an offset sees the text before it: `^` does not match
// there, `\b` and lookbehind look at the character before.
TEST(Regex, SearchesFromAnOffsetSeeThePrecedingText) {
    EXPECT_EQ(allMatches("^\\w", "ab\ncd"), "[a]");
    EXPECT_EQ(allMatches("(?m)^\\w", "ab\ncd"), "[a][c]");
    EXPECT_EQ(allMatches("(?m)^\\w\\n?", "a\nb\nc"), "[a\n][b\n][c]");
    EXPECT_EQ(allMatches("\\b\\w", "ab cd"), "[a][c]");
    EXPECT_EQ(allMatches("(?<=a)b", "abab"), "[b][b]");
    EXPECT_EQ(allMatches("\\w", "ab"), "[a][b]");
    Match m;
    const std::string s = "xa ya";
    ASSERT_TRUE(protoClojure::regexSearch(compilePattern("\\ba"), "a xa", 0, m));
    EXPECT_FALSE(protoClojure::regexSearch(compilePattern("\\ba"), s, 2, m));
    EXPECT_FALSE(protoClojure::regexSearch(compilePattern("a"), s, s.size() + 1, m));
}

// After an empty match Java's Matcher.find starts one character further on;
// it does not retry a non-empty match at the same place.
TEST(Regex, SuccessiveMatchesWalkAsJavasFind) {
    EXPECT_EQ(allMatches("x*", "ab"), "[][][]");
    EXPECT_EQ(allMatches("a*", "baaa"), "[][aaa][]");
    EXPECT_EQ(allMatches("|a", "a"), "[][]");
    EXPECT_EQ(allMatches("\\d+", "1 22 333"), "[1][22][333]");
}

TEST(Regex, GroupsThatDidNotTakePartAreUnset) {
    Match m;
    const std::string s = "xa";
    ASSERT_TRUE(protoClojure::regexSearch(compilePattern("(a)(b)?(c)?"), s, 0, m));
    EXPECT_TRUE(m.matched(1));
    EXPECT_FALSE(m.matched(2));
    EXPECT_FALSE(m.matched(3));
    EXPECT_EQ(m.str(s, 1), "a");
}

TEST(Regex, LongSubjectsMatchOnTheJitStack) {
    const std::string big(300000, 'a');
    EXPECT_EQ(firstMatch("(a|b)*", big).size(), big.size());
    EXPECT_EQ(firstMatch("(a)\\1*", big).size(), big.size());
}

TEST(Regex, TheEngineGivingUpIsAStackOverflowErrorNotANoMatch) {
    // Catastrophic backtracking under a lowered match limit.
    protoClojure::setRegexMatchLimit(100000);
    const std::string subject = std::string(40, 'a') + "!";
    EXPECT_THROW(firstMatch("(a|aa)+$", subject), protoClojure::StackOverflowError);
    protoClojure::setRegexMatchLimit(protoClojure::kRegexMatchLimit);
    // The JIT stack: `(a|b)*` takes 32 bytes of it per character.
    const std::string huge(protoClojure::kJitStackMaxBytes / 16, 'a');
    EXPECT_THROW(firstMatch("(a|b)*", huge), protoClojure::StackOverflowError);
}

TEST(Regex, PatternValuesKeepTheirSource) {
    proto::ProtoSpace space;
    proto::ProtoContext* ctx = space.rootContext;
    protoClojure::initRegex(ctx);
    const proto::ProtoObject* p = protoClojure::makePattern(ctx, "x(\\d)");
    ASSERT_TRUE(protoClojure::isPattern(ctx, p));
    EXPECT_FALSE(protoClojure::isMatcher(ctx, p));
    EXPECT_EQ(protoClojure::patternSource(ctx, p), "x(\\d)");
    EXPECT_EQ(protoClojure::patternOf(ctx, p), compilePattern("x(\\d)"));
}
