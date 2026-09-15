// Unit tests for BytecodeModule (src/runtime/BytecodeModule.h): constant
// de-duplication by kind and value, and the 24-bit instruction operand. The
// conformance fixtures and tests/cli/large-program.sh cover the programs
// these limits allow.

#include "runtime/BytecodeModule.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

using protoClojure::BytecodeModule;
using protoClojure::kMaxOperand;
using protoClojure::kOperandShift;
using protoClojure::Op;

TEST(BytecodeModule, EqualConstantsOfOneKindShareAnEntry) {
    BytecodeModule m;
    const std::size_t one = m.addLong(1);
    EXPECT_EQ(m.addLong(1), one);
    EXPECT_EQ(m.addDouble(2.5), m.addDouble(2.5));
    EXPECT_EQ(m.addString("a"), m.addString("a"));
    EXPECT_EQ(m.addSymbol("f"), m.addSymbol("f"));
    EXPECT_EQ(m.addNamed(":a", nullptr), m.addNamed(":a", nullptr));
    EXPECT_EQ(m.addBigInteger("12345678901234567890"),
              m.addBigInteger("12345678901234567890"));
    const double nan = std::numeric_limits<double>::quiet_NaN();
    EXPECT_EQ(m.addDouble(nan), m.addDouble(nan));
    EXPECT_EQ(m.constCount(), 7u);
}

TEST(BytecodeModule, ConstantsOfDifferentKindsOrBitsStayDistinct) {
    BytecodeModule m;
    const std::set<std::size_t> indices{
        m.addLong(1),       m.addDouble(1.0),     m.addString("1"),
        m.addString("a"),   m.addString(":a"),    m.addNamed(":a", nullptr),
        m.addNamed("a", nullptr), m.addSymbol("a"), m.addDouble(0.0),
        m.addDouble(-0.0),  m.addLong(0)};
    EXPECT_EQ(indices.size(), 11u);
    EXPECT_EQ(m.constCount(), 11u);
    EXPECT_EQ(m.constAt(m.addLong(1)).kind, BytecodeModule::ConstKind::Long);
    EXPECT_EQ(m.constAt(m.addDouble(1.0)).kind, BytecodeModule::ConstKind::Double);
    EXPECT_TRUE(std::signbit(m.constAt(m.addDouble(-0.0)).dval));
}

TEST(BytecodeModule, ManyDistinctConstantsGetSequentialIndices) {
    BytecodeModule m;
    for (long long i = 0; i < 70000; ++i)
        ASSERT_EQ(m.addLong(i), static_cast<std::size_t>(i));
    EXPECT_EQ(m.addLong(69999), 69999u);
    EXPECT_EQ(m.constCount(), 70000u);
}

TEST(BytecodeModule, OperandsUseTwentyFourBits) {
    BytecodeModule m;
    const std::size_t at = m.emit(Op::PUSH_CONST, kMaxOperand);
    ASSERT_EQ(m.code().size(), 1u);
    EXPECT_EQ(m.code()[at] & 0xFF, static_cast<std::uint32_t>(Op::PUSH_CONST));
    EXPECT_EQ(m.code()[at] >> kOperandShift, kMaxOperand);
    EXPECT_THROW(m.emit(Op::PUSH_CONST, std::size_t{kMaxOperand} + 1),
                 std::length_error);

    const std::size_t jump = m.emit(Op::JUMP, 0);
    m.patchOperand(jump, 70000);
    EXPECT_EQ(m.code()[jump] & 0xFF, static_cast<std::uint32_t>(Op::JUMP));
    EXPECT_EQ(m.code()[jump] >> kOperandShift, 70000u);
    EXPECT_THROW(m.patchOperand(jump, std::size_t{kMaxOperand} + 1),
                 std::length_error);
    EXPECT_THROW(m.patchOperand(m.pos(), 0), std::out_of_range);
}
