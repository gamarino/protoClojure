/*
 * Opcodes — the v0.0.x bytecode VM instruction set.
 *
 * Each instruction is one 32-bit word (Instr): the opcode in the low 8 bits
 * and an unsigned 24-bit operand in the high 24 bits, so every operand —
 * constant-pool index, local slot, block or arity-group index, argument
 * count, jump offset (counted in instructions) — ranges over
 * 0..kMaxOperand (16,777,215). BytecodeModule::emit rejects a larger
 * operand. One fixed width keeps decoding to a single load and a shift and
 * lets forward jumps be back-patched in place.
 *
 * Session 3 minimum — only what `(println "hello, world")` needs:
 *
 *   PUSH_CONST  push consts[operand] onto the operand stack
 *   PUSH_VAR    push (globals get consts[operand].symbol) onto the stack
 *   CALL        invoke top-of-stack callable with `operand` args below it
 *   POP         drop the top of stack (for statement-level discard)
 *   RETURN      end of bytecode; return top of stack (or PROTO_NONE)
 */
#pragma once
#include <cstddef>
#include <cstdint>

namespace protoClojure {

enum class Op : uint8_t {
    NOP             = 0,
    PUSH_CONST      = 1,
    PUSH_VAR        = 2,
    CALL            = 3,
    POP             = 4,
    RETURN          = 5,

    // Session 4 additions:
    STORE_GLOBAL    = 6,   // operand = const-pool symbol idx; pops TOS, sets
                           //           (globals symName) = value. Re-pushes
                           //           the stored value as the def's result.
    JUMP            = 7,   // operand = forward offset in instructions (×2 bytes)
    JUMP_IF_FALSE   = 8,   // pop; if value is nil or false, jump operand instrs.
    PUSH_NIL        = 9,   // push PROTO_NONE (the Clojure nil)
    PUSH_TRUE       = 10,
    PUSH_FALSE      = 11,

    // Session 5 additions:
    PUSH_LOCAL      = 12,  // operand = local-slot index; push frame.local[i]
    STORE_LOCAL     = 13,  // operand = local-slot index; pop, write into slot
    MAKE_FN         = 14,  // operand = block-index in parent module; push
                           //           a callable fn-wrapper for that body
    JUMP_BACK       = 15,  // operand = BACKWARD offset (subtracted from pc)
                           //           used by loop/recur

    // Session 7 additions:
    CALL_APPLY      = 16,  // stack: [callable, args-list]. Spread the list
                           //        as positional args and dispatch like
                           //        CALL with argc = list size. Operand
                           //        reserved (must be 0 in v0.7.x).

    // Session 8 additions:
    DUP             = 17,  // copy top of stack (used by and/or short-circuit)
    JUMP_IF_TRUE    = 18,  // pop; jump forward if value is NOT (nil|false)
    MAKE_FN_MULTI   = 19,  // operand = arityGroup index in parent module;
                           //           wraps N arities into a single multi-
                           //           arity fn. Pops sum-of-captureCounts
                           //           values off the stack (arity 0 caps
                           //           first, then arity 1, ...).

    // Session 11 additions — SmallInt fast-path binary arithmetic and
    // comparison. Each opcode pops two operands, pushes one result.
    // The fast path assumes both operands are tagged SmallInt; for any
    // other shape (float, large int, type mismatch) we fall back to the
    // matching primitive. The compiler emits these ONLY for the standard
    // operator symbols and ONLY when those symbols are not shadowed by a
    // local binding — so the call-site semantics under shadowing stay
    // unchanged. Operand byte is reserved (must be 0).
    ADD             = 20,
    SUB             = 21,
    MUL             = 22,
    LT              = 23,
    LE              = 24,
    GT              = 25,
    GE              = 26,
    EQ              = 27,

    // Session 14 — call whose source ends in `:keyword value` pairs.
    // Stack: [callable, arg1, ..., argN], operand = N, exactly as for
    // CALL. A callee that is not keyword-based receives the N arguments
    // unchanged (same as CALL). A keyword-based callee of arity F takes
    // arg1..argF as positionals and requires the remaining N - F
    // arguments to be key/value pairs, which the VM folds into the kwArgs
    // map in order (last duplicate wins) and passes like the session-13
    // trailing-map path.
    CALL_KW         = 28,
};

// One instruction word: opcode in the low byte, operand in the high 24 bits.
// Code positions and jump offsets count instruction words.
using Instr = std::uint32_t;
inline constexpr unsigned      kOperandShift = 8;
inline constexpr std::uint32_t kMaxOperand   = (1u << 24) - 1;  // 16,777,215

const char* opName(Op op);

} // namespace protoClojure
