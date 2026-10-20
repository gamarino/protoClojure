// Unit tests for BytecodeModule (src/runtime/BytecodeModule.h): constant
// de-duplication by kind and value, and the 24-bit instruction operand. The
// conformance fixtures and tests/cli/large-program.sh cover the programs
// these limits allow.

#include "runtime/BytecodeModule.h"

#include "protoCore.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

using protoClojure::BytecodeModule;
using protoClojure::kMaxOperand;
using protoClojure::kOperandShift;
using protoClojure::Op;

namespace {
// Symbol constants are interned when added, which needs a protoCore context.
proto::ProtoContext* symbolContext() {
    static proto::ProtoSpace space;
    return space.rootContext;
}
}

TEST(BytecodeModule, EqualConstantsOfOneKindShareAnEntry) {
    BytecodeModule m;
    const std::size_t one = m.addLong(1);
    EXPECT_EQ(m.addLong(1), one);
    EXPECT_EQ(m.addDouble(2.5), m.addDouble(2.5));
    EXPECT_EQ(m.addString("a"), m.addString("a"));
    EXPECT_EQ(m.addSymbol(symbolContext(), "f"), m.addSymbol(symbolContext(), "f"));
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
        m.addNamed("a", nullptr), m.addSymbol(symbolContext(), "a"), m.addDouble(0.0),
        m.addDouble(-0.0),  m.addLong(0)};
    EXPECT_EQ(indices.size(), 11u);
    EXPECT_EQ(m.constCount(), 11u);
    EXPECT_EQ(m.constAt(m.addLong(1)).kind, BytecodeModule::ConstKind::Long);
    EXPECT_EQ(m.constAt(m.addDouble(1.0)).kind, BytecodeModule::ConstKind::Double);
    EXPECT_TRUE(std::signbit(m.constAt(m.addDouble(-0.0)).dval));
}

TEST(BytecodeModule, SymbolConstantsCarryTheirInternedKey) {
    BytecodeModule m;
    proto::ProtoContext* ctx = symbolContext();
    const std::size_t shortName = m.addSymbol(ctx, "assoc");
    const std::size_t longName = m.addSymbol(ctx, "a-global-name-longer-than-an-inline-string");
    EXPECT_EQ(m.constAt(shortName).named,
              reinterpret_cast<const proto::ProtoObject*>(proto::ProtoString::createSymbol(ctx, "assoc")));
    EXPECT_EQ(m.constAt(longName).named,
              reinterpret_cast<const proto::ProtoObject*>(proto::ProtoString::createSymbol(
                  ctx, "a-global-name-longer-than-an-inline-string")));
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

// maxStack: the deepest point of the operand stack, from a flow analysis of
// the finished code (BytecodeModule::computeMaxStack).  Hand-assembled
// bodies with known depths.

TEST(BytecodeModuleMaxStack, StraightLineCountsTheDeepestPush) {
    BytecodeModule m;
    const auto c = m.addLong(1);
    m.emit(Op::PUSH_CONST, c);
    m.emit(Op::PUSH_CONST, c);
    m.emit(Op::ADD, 0);            // two in, one out
    m.emit(Op::RETURN, 0);
    EXPECT_EQ(m.maxStack(), 2u);
}

TEST(BytecodeModuleMaxStack, ACallHoldsTheCallableAndItsArguments) {
    BytecodeModule m;
    const auto f = m.addSymbol(symbolContext(), "f");
    const auto c = m.addLong(1);
    m.emit(Op::PUSH_VAR, f);
    for (int i = 0; i < 3; ++i) m.emit(Op::PUSH_CONST, c);
    m.emit(Op::CALL, 3);           // callable and three arguments in, the result out
    m.emit(Op::PUSH_CONST, c);
    m.emit(Op::ADD, 0);
    m.emit(Op::RETURN, 0);
    EXPECT_EQ(m.maxStack(), 4u);
}

TEST(BytecodeModuleMaxStack, BranchesKeepTheDeeperPath) {
    BytecodeModule m;
    const auto c = m.addLong(1);
    m.emit(Op::PUSH_TRUE, 0);          // 0: depth 0 -> 1
    m.emit(Op::JUMP_IF_FALSE, 2);      // 1: pops; else at 4, depth 0
    m.emit(Op::PUSH_CONST, c);         // 2: then: 0 -> 1
    m.emit(Op::JUMP, 3);               // 3: to 7
    m.emit(Op::PUSH_CONST, c);         // 4: else: 0 -> 1
    m.emit(Op::PUSH_CONST, c);         // 5: 1 -> 2, the deepest point
    m.emit(Op::ADD, 0);                // 6: 2 -> 1
    m.emit(Op::RETURN, 0);             // 7
    EXPECT_EQ(m.maxStack(), 2u);
}

TEST(BytecodeModuleMaxStack, ALoopBackEdgeTerminatesTheAnalysis) {
    BytecodeModule m;
    const auto c = m.addLong(1);
    m.emit(Op::PUSH_CONST, c);         // 0: 0 -> 1
    m.emit(Op::POP, 0);                // 1: 1 -> 0
    m.emit(Op::JUMP_BACK, 3);          // 2: back to 0
    EXPECT_EQ(m.maxStack(), 1u);
}

TEST(BytecodeModuleMaxStack, AHandlerStartsOneDeeper) {
    BytecodeModule m;
    const auto c = m.addLong(1);
    m.emit(Op::PUSH_CONST, c);         // 0: 0 -> 1
    m.emit(Op::TRY_BEGIN, 2);          // 1: handler at 4, entered at depth 2
    m.emit(Op::TRY_END, 0);            // 2
    m.emit(Op::RETURN, 0);             // 3
    m.emit(Op::POP, 0);                // 4: the exception
    m.emit(Op::RETURN, 0);             // 5
    EXPECT_EQ(m.maxStack(), 2u);
}

TEST(BytecodeModuleMaxStack, AnEmptyBodyNeedsNoStack) {
    BytecodeModule m;
    m.emit(Op::RETURN, 0);
    EXPECT_EQ(m.maxStack(), 0u);
}
