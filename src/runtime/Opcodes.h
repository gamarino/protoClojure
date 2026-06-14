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
    NOP        = 0,
    PUSH_CONST = 1,
    PUSH_VAR   = 2,
    CALL       = 3,
    POP        = 4,
    RETURN     = 5,
};

inline constexpr std::size_t kInstrSize = 2;

const char* opName(Op op);

} // namespace protoClojure
