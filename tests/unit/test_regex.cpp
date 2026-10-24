// Unit tests for the regular-expression layer (src/runtime/Regex.h): the
// translation of Java's dialect to the engine's, the compiled-pattern cache,
// the choice of executor, and pattern values.

#include "runtime/Regex.h"

#include "protoCore.h"
#include <gtest/gtest.h>

#include <regex>
#include <string>

using protoClojure::compilePattern;
using protoClojure::PatternSyntaxError;

namespace {

std::string firstMatch(const char* pattern, const std::string& subject) {
    std::smatch m;
    if (!std::regex_search(subject, m, compilePattern(pattern)->regex)) return "<none>";
    return m.str(0);
}

} // namespace

TEST(Regex, CacheReturnsOneCompiledPatternPerSource) {
    const auto* a = compilePattern("a+b");
    EXPECT_EQ(a, compilePattern("a+b"));
    EXPECT_NE(a, compilePattern("a+c"));
    EXPECT_EQ(a->source, "a+b");
    EXPECT_EQ(compilePattern("(a)(?:b)(c)")->groups, 2u);
}

TEST(Regex, JavaInlineFlagsAndConstructsAreTranslated) {
    EXPECT_EQ(firstMatch("(?i)ABC", "xabcx"), "abc");
    EXPECT_EQ(firstMatch("(?s)a.b", "a\nb"), "a\nb");
    EXPECT_EQ(firstMatch("a.b", "a\nb"), "<none>");
    EXPECT_EQ(firstMatch("(?is)A.B", "a\nb"), "a\nb");
    EXPECT_EQ(firstMatch("(?<word>\\w+)!", "hi!"), "hi!");
    EXPECT_EQ(compilePattern("(?<word>\\w+)!")->groups, 1u);
    EXPECT_EQ(firstMatch("\\Q*.[\\E", "a*.[b"), "*.[");
    EXPECT_EQ(firstMatch("\\Aab\\z", "ab"), "ab");
    EXPECT_EQ(firstMatch("[.]", "a.b"), ".");          // a dot in a class stays a dot
    EXPECT_EQ(firstMatch("(?s)[.]", "a\nb.c"), ".");
}

TEST(Regex, UnsupportedSyntaxIsAPatternSyntaxError) {
    EXPECT_THROW(compilePattern("(ab"), PatternSyntaxError);
    EXPECT_THROW(compilePattern("(?x)a b"), PatternSyntaxError);
    EXPECT_THROW(compilePattern("(?<=a)b"), PatternSyntaxError);
}

TEST(Regex, BackReferencesSelectTheBacktrackingExecutor) {
#if defined(_LIBCPP_VERSION) || defined(_MSVC_STL_VERSION)
    // libc++ backtracks on a heap vector, never on the native stack; MSVC
    // bounds its own recursion and throws (runRegex).
    EXPECT_FALSE(compilePattern("(a)\\1")->recursive);
    EXPECT_FALSE(compilePattern("(a|b)*")->recursive);
    return;
#endif
    EXPECT_TRUE(compilePattern("(a)\\1")->recursive);
#if defined(__GLIBCXX__)
    EXPECT_FALSE(compilePattern("(a|b)*")->recursive);
#endif
    // A short subject passes the guard; a huge one is refused before matching.
    EXPECT_NO_THROW(protoClojure::guardRecursiveMatch(compilePattern("(a)\\1"), 100));
    EXPECT_ANY_THROW(protoClojure::guardRecursiveMatch(compilePattern("(a)\\1"), std::size_t(1) << 40));
}

// A continued search (re-seq, replace, split) sees the text before its start:
// `^` does not match there, `\b` looks at the byte before. libc++ needed its
// own flags for this (continuationFlags).
TEST(Regex, ContinuedSearchesSeeThePrecedingText) {
    auto all = [](const char* pattern, const std::string& s) {
        std::string out;
        protoClojure::forEachMatch(s, compilePattern(pattern), [&](const std::smatch& m) {
            out += "[" + m.str(0) + "]";
            return true;
        });
        return out;
    };
#if defined(_MSVC_STL_VERSION)
    // MSVC's ECMAScript `^` also matches after a line terminator, with or
    // without (?m) (STATUS.md, known issues).
    EXPECT_EQ(all("^\\w", "ab\ncd"), "[a][c]");
#else
    EXPECT_EQ(all("^\\w", "ab\ncd"), "[a]");
#endif
    EXPECT_EQ(all("\\b\\w", "ab cd"), "[a][c]");
    EXPECT_EQ(all("\\w", "ab"), "[a][b]");
    EXPECT_EQ(all("x*", "ab"), "[][][]");
#if !defined(_MSVC_STL_VERSION)
    EXPECT_EQ(all("(?m)^\\w", "ab\ncd"), "[a][c]");
    EXPECT_EQ(all("(?m)^\\w\\n?", "a\nb\nc"), "[a\n][b\n][c]");
#endif
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
