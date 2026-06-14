#include "ExecutionEngine.h"
#include "BytecodeModule.h"
#include "Opcodes.h"

#include "protoCore.h"

#include <stdexcept>

namespace protoClojure {

namespace {

// Per-frame stack size. Plenty for v0.0.x; the slot region can grow via
// resizeAutomaticLocals if a future frame needs more. Sized at 64 because
// the worst hello-world frame uses ~3 slots.
constexpr unsigned int kFrameStackSize = 64;

// Materialise one const-pool entry into a ProtoObject. Allocation goes via
// the supplied context — meaning the result's natural rooting is on the
// caller's frame, where it will be stored into a slot immediately by the
// PUSH_CONST handler.
const proto::ProtoObject* materialise(proto::ProtoContext* ctx,
                                       const BytecodeModule::Const& c) {
    using K = BytecodeModule::ConstKind;
    switch (c.kind) {
        case K::Long:
            return ctx->fromLong(c.ival);
        case K::String:
            return ctx->fromUTF8String(c.sval.c_str());
        case K::Symbol:
            // Materialised symbols are interned — same pointer for the same
            // name across the whole ProtoSpace. The globals table is keyed
            // by these pointers, so identity equality short-circuits hash.
            return reinterpret_cast<const proto::ProtoObject*>(
                proto::ProtoString::createSymbol(ctx, c.sval.c_str()));
    }
    return PROTO_NONE;
}

} // namespace

const proto::ProtoObject*
ExecutionEngine::run(proto::ProtoContext* parent,
                     const BytecodeModule& mod,
                     const proto::ProtoObject* globals) {
    // One ProtoContext per frame. Its automaticLocals is the operand stack.
    proto::ProtoContext frame(parent->space, parent);
    frame.resizeAutomaticLocals(kFrameStackSize);
    unsigned int sp = 0;

    const auto& bytes = mod.bytes();
    std::size_t pc = 0;

    auto push = [&](const proto::ProtoObject* v) {
        if (sp >= kFrameStackSize) {
            throw std::runtime_error("VM: operand-stack overflow");
        }
        frame.setAutomaticLocal(sp++, v);
    };
    auto pop = [&]() -> const proto::ProtoObject* {
        if (sp == 0) {
            throw std::runtime_error("VM: operand-stack underflow");
        }
        return frame.getAutomaticLocal(--sp);
    };
    (void)pop;  // silence unused; the per-opcode use is below

    while (pc + 1 < bytes.size()) {
        Op op = static_cast<Op>(bytes[pc]);
        std::uint8_t operand = bytes[pc + 1];
        pc += kInstrSize;

        switch (op) {
            case Op::NOP:
                break;

            case Op::PUSH_CONST: {
                if (operand >= mod.constCount()) {
                    throw std::runtime_error("VM: PUSH_CONST out-of-range");
                }
                push(materialise(&frame, mod.constAt(operand)));
                break;
            }

            case Op::PUSH_VAR: {
                if (operand >= mod.constCount()) {
                    throw std::runtime_error("VM: PUSH_VAR out-of-range");
                }
                const auto& c = mod.constAt(operand);
                if (c.kind != BytecodeModule::ConstKind::Symbol) {
                    throw std::runtime_error("VM: PUSH_VAR const is not a symbol");
                }
                const proto::ProtoString* key =
                    proto::ProtoString::createSymbol(&frame, c.sval.c_str());
                const proto::ProtoObject* v =
                    globals->getAttribute(&frame, key);
                if (!v || v == PROTO_NONE) {
                    throw std::runtime_error(
                        "VM: unable to resolve symbol: " + c.sval);
                }
                push(v);
                break;
            }

            case Op::CALL: {
                unsigned int argc = operand;
                if (sp < argc + 1u) {
                    throw std::runtime_error(
                        "VM: CALL with insufficient stack");
                }
                // The callable sits at stack depth argc (top-of-stack is the
                // LAST argument). Layout immediately before CALL:
                //   sp-1 - argc → callable
                //   sp - argc   → arg 0
                //   sp - argc+1 → arg 1
                //   ...
                //   sp - 1      → arg N-1
                //
                // We build a fresh args ProtoList in a child context so it
                // stays GC-rooted for the duration of the primitive call.
                proto::ProtoContext callScope(frame.space, &frame);
                callScope.resizeAutomaticLocals(2);
                constexpr unsigned int kSlotArgs     = 0;
                constexpr unsigned int kSlotCallable = 1;
                callScope.setAutomaticLocal(
                    kSlotArgs, callScope.newList()->asObject(&callScope));
                callScope.setAutomaticLocal(
                    kSlotCallable, frame.getAutomaticLocal(sp - 1 - argc));

                for (unsigned int i = 0; i < argc; ++i) {
                    const proto::ProtoObject* arg =
                        frame.getAutomaticLocal(sp - argc + i);
                    const proto::ProtoList* cur =
                        callScope.getAutomaticLocal(kSlotArgs)->asList(&callScope);
                    const proto::ProtoList* updated =
                        cur->appendLast(&callScope, arg);
                    callScope.setAutomaticLocal(
                        kSlotArgs, updated->asObject(&callScope));
                }

                const proto::ProtoObject* callable =
                    callScope.getAutomaticLocal(kSlotCallable);
                proto::ProtoMethod fn = callable->asMethod(&callScope);
                if (!fn) {
                    throw std::runtime_error(
                        "VM: value is not callable in CALL");
                }
                const proto::ProtoObject* self =
                    callable->asMethodSelf(&callScope);
                const proto::ProtoList* argsList =
                    callScope.getAutomaticLocal(kSlotArgs)->asList(&callScope);

                const proto::ProtoObject* result =
                    fn(&callScope, self, nullptr, argsList, nullptr);

                // Drop the args + callable from the frame's operand stack,
                // then push the result. We do this in a single statement
                // sequence — no allocation between the call's return and
                // the result's installation into the frame's slot.
                sp -= (argc + 1);
                push(result);
                break;
            }

            case Op::POP: {
                if (sp == 0) {
                    throw std::runtime_error("VM: POP on empty stack");
                }
                --sp;
                break;
            }

            case Op::RETURN: {
                if (sp == 0) return PROTO_NONE;
                return frame.getAutomaticLocal(sp - 1);
            }

            default:
                throw std::runtime_error("VM: unknown opcode");
        }
    }
    // Off the end without RETURN: return whatever is on top, or nil.
    return (sp == 0) ? PROTO_NONE : frame.getAutomaticLocal(sp - 1);
}

} // namespace protoClojure
