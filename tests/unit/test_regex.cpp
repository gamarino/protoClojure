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
    EXPECT_TRUE(compilePattern("(a)\\1")->recursive);
#if defined(__GLIBCXX__)
    EXPECT_FALSE(compilePattern("(a|b)*")->recursive);
#endif
    // A short subject passes the guard; a huge one is refused before matching.
    EXPECT_NO_THROW(protoClojure::guardRecursiveMatch(compilePattern("(a)\\1"), 100));
    EXPECT_ANY_THROW(protoClojure::guardRecursiveMatch(compilePattern("(a)\\1"), std::size_t(1) << 40));
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
