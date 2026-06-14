/*
 * Opcodes — the v0.0.x bytecode VM instruction set.
 *
 * Each instruction is two bytes: opcode + one-byte operand. Operand
 * widening (>255 const-pool indices, etc.) lands in session 4+ via an
 * EXTEND prefix — same pattern protoST uses.
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

    // Session 14 — trailing-kv-pair call. Stack: [callable, pos1,
    // ..., posN, kwMap]. operand = N + 1 (counting kwMap as the last
    // positional, same way CALL counts its args). The VM decides at
    // runtime: when the callee declares `isKwBased` it keeps the
    // kwMap and dispatches it like the session-13 trailing-map path;
    // otherwise it unpacks the kwMap into `k1 v1 k2 v2 ...`
    // positionals and dispatches with the expanded argc. This
    // preserves call semantics for non-kw callees (`(println :a 1)`
    // still prints `:a 1`) while letting kw-based callees get the
    // map directly.
    CALL_KW         = 28,
};

inline constexpr std::size_t kInstrSize = 2;

const char* opName(Op op);

} // namespace protoClojure
