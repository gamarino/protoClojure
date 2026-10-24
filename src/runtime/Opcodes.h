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

    // Exceptions (try / catch / finally / throw; docs/DESIGN.md §
    // "Exceptions"). A frame keeps a stack of active handlers; each records
    // where its handler code starts and the operand-stack depth at entry.
    TRY_BEGIN       = 29,  // operand = FORWARD offset to the handler code.
                           //           Pushes a handler. When an exception
                           //           escapes any instruction (or call)
                           //           while the handler is active, the VM
                           //           pops it, truncates the operand stack
                           //           to the recorded depth, pushes the
                           //           exception value and jumps there.
    TRY_END         = 30,  // pops the innermost handler (normal exit).
    THROW           = 31,  // pop; raise the value as an exception. A value
                           //       that `throw` does not accept (neither a
                           //       Clojure exception nor a foreign object,
                           //       Exceptions.h isThrowable) raises a
                           //       ClassCastException instead.
    EXC_MATCH       = 32,  // operand = built-in exception class id
                           //           (Exceptions.h). pop; push true when a
                           //           catch of that class handles the value
                           //           (exceptionMatches).

    // Unified exceptions and module members (docs/DESIGN.md §4.1, §4.3).
    RETHROW         = 33,  // operand = local slot written by STORE_CAUGHT.
                           //       Rethrows the ORIGINAL C++ exception the
                           //       handler caught (std::rethrow_exception),
                           //       so a runtime subclass of ProtoException
                           //       keeps its type and extras; when the
                           //       handler caught a translated C++ error,
                           //       raises the slot's value as a new
                           //       ProtoException. The rethrow of a value no
                           //       catch clause handled, and the end of a
                           //       finally handler. Reads no stack.
    EXC_MATCH_TYPE  = 34,  // operand = const-pool string idx (the clause's
                           //           class name, for messages). Stack:
                           //           [value, type]. pop both; push true
                           //           when `type` is on the value's
                           //           prototype chain. A `type` that is not
                           //           a prototype object raises
                           //           IllegalArgumentException.
    RESOLVE_CLASS   = 35,  // operand = const-pool symbol idx. Push the
                           //           catch type the symbol names at run
                           //           time: the global `X`, or member `b`
                           //           of the global `a` for `a/b`. A name
                           //           that does not resolve raises
                           //           "Unable to resolve classname: X".
    GET_MEMBER      = 36,  // operand = const-pool symbol idx. pop an object;
                           //           push its attribute of that name (its
                           //           prototype chain included). A missing
                           //           member raises RuntimeException.
    CALL_MEMBER     = 37,  // operand = argc; the NEXT word is a NOP whose
                           //           operand is the const-pool symbol idx
                           //           of the member (skipped by the VM).
                           //           Stack: [receiver, arg1..argN]. Calls
                           //           the receiver's member with the
                           //           receiver as `self` (a protoCore
                           //           method) or as a Clojure fn; pops
                           //           receiver and args, pushes the result.
    STORE_CAUGHT    = 38,  // operand = local slot. The first instruction of
                           //           a handler: pop the caught value into
                           //           the slot, and keep the caught C++
                           //           exception (std::exception_ptr) as
                           //           frame state for that slot's RETHROW.

    // Lazy sequences (docs/DESIGN.md §10.2, M30).
    PUSH_LOCAL_CLEAR = 39, // operand = local slot. As PUSH_LOCAL, then clears
                           //           the slot: the compiler emits it for
                           //           the one read of a local that no later
                           //           instruction reads again (locals
                           //           clearing), so a local bound to a lazy
                           //           seq does not hold its head while the
                           //           seq is walked.
};

// One instruction word: opcode in the low byte, operand in the high 24 bits.
// Code positions and jump offsets count instruction words.
using Instr = std::uint32_t;
inline constexpr unsigned      kOperandShift = 8;
inline constexpr std::uint32_t kMaxOperand   = (1u << 24) - 1;  // 16,777,215

const char* opName(Op op);

} // namespace protoClojure
