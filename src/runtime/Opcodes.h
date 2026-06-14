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
};

inline constexpr std::size_t kInstrSize = 2;

const char* opName(Op op);

} // namespace protoClojure
