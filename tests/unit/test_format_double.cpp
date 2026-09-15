// Unit tests for the double printer (formatDouble in
// src/runtime/Primitives.h). The expected strings are what JVM Clojure
// prints, that is Java's Double.toString (JDK 19 and later, which selects
// the shortest decimal that rounds to the double). The conformance fixtures
// under tests/conformance/13-floats cover println, str and collections.

#include "runtime/Primitives.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
#include <string>

namespace {

using protoClojure::formatDouble;

TEST(FormatDouble, MatchesJavaDoubleToString) {
    struct Case { double value; const char* expected; };
    const Case cases[] = {
        {0.0, "0.0"},
        {-0.0, "-0.0"},
        {1.0, "1.0"},
        {-2.5, "-2.5"},
        {100.0, "100.0"},
        {3.14, "3.14"},
        {1.0 / 3, "0.3333333333333333"},
        {2.0 / 3, "0.6666666666666666"},
        {0.1 + 0.2, "0.30000000000000004"},
        {0.001, "0.001"},
        {0.0001, "1.0E-4"},
        {1.5e-7, "1.5E-7"},
        {1234567.5, "1234567.5"},
        {9999999.0, "9999999.0"},
        {1e7, "1.0E7"},
        {1e16, "1.0E16"},
        {1e21, "1.0E21"},
        {-1e19, "-1.0E19"},
        {123456789012345680000.0, "1.2345678901234568E20"},
        {9.223372036854775807e18, "9.223372036854776E18"},
        {1e300, "1.0E300"},
        {std::numeric_limits<double>::max(), "1.7976931348623157E308"},
        {std::numeric_limits<double>::min(), "2.2250738585072014E-308"},
        // The smallest subnormals: the shortest decimal has one digit
        // (5E-324, 1E-323), and Java picks the closest two-digit decimal.
        {std::numeric_limits<double>::denorm_min(), "4.9E-324"},
        {2 * std::numeric_limits<double>::denorm_min(), "9.9E-324"},
        {std::numeric_limits<double>::infinity(), "##Inf"},
        {-std::numeric_limits<double>::infinity(), "##-Inf"},
        {std::numeric_limits<double>::quiet_NaN(), "##NaN"},
        {-std::numeric_limits<double>::quiet_NaN(), "##NaN"},
    };
    for (const Case& c : cases) {
        EXPECT_EQ(formatDouble(c.value), c.expected) << "value " << c.value;
    }
}

// Every finite double reads back as itself (strtod accepts the E notation),
// and the output uses plain notation exactly for magnitudes in [1e-3, 1e7).
TEST(FormatDouble, RoundTripsAndChoosesNotationByMagnitude) {
    std::mt19937_64 rng(20260915);
    for (int i = 0; i < 200000; ++i) {
        std::uint64_t bits = rng();
        // Half of the samples in the plain-notation range and its borders.
        if (i % 2 == 0) {
            const double scaled = std::ldexp(static_cast<double>(bits >> 11), -53);
            const double v = std::pow(10.0, -4.0 + 12.0 * scaled);
            std::memcpy(&bits, &v, sizeof bits);
        }
        double d = 0;
        std::memcpy(&d, &bits, sizeof d);
        if (!std::isfinite(d)) continue;
        const std::string s = formatDouble(d);
        char* end = nullptr;
        const double back = std::strtod(s.c_str(), &end);
        ASSERT_EQ(*end, '\0') << s;
        ASSERT_EQ(std::memcmp(&back, &d, sizeof d), 0) << s;
        const double m = std::fabs(d);
        const bool plain = s.find('E') == std::string::npos;
        ASSERT_EQ(plain, m == 0.0 || (m >= 1e-3 && m < 1e7)) << s;
        ASSERT_NE(s.find('.'), std::string::npos) << s;
    }
}

} // namespace
