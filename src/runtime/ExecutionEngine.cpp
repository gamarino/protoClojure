#include "ExecutionEngine.h"
#include "BytecodeModule.h"
#include "Opcodes.h"

#include "protoCore.h"

#include <cstdio>
#include <stdexcept>

namespace protoClojure {

namespace {

// Per-frame stack size budget. Locals (params + let-bindings) live in slots
// 0..(arity + localCount - 1); the operand stack lives starting at offset
// arity + localCount.
constexpr unsigned int kOperandStackSize = 64;

const proto::ProtoObject* materialise(proto::ProtoContext* ctx,
                                       const BytecodeModule::Const& c) {
    using K = BytecodeModule::ConstKind;
    switch (c.kind) {
        case K::Long:   return ctx->fromLong(c.ival);
        case K::String: return ctx->fromUTF8String(c.sval.c_str());
        case K::Symbol:
            return reinterpret_cast<const proto::ProtoObject*>(
                proto::ProtoString::createSymbol(ctx, c.sval.c_str()));
    }
    return PROTO_NONE;
}

} // namespace

const proto::ProtoObject*
ExecutionEngine::run(proto::ProtoContext* parent,
                     const BytecodeModule& mod,
                     const proto::ProtoObject* globals,
                     const proto::ProtoObject* fnMarkerProto,
                     const proto::ProtoString* bytecodeKey,
                     const proto::ProtoString* arityKey,
                     const proto::ProtoString* capturesKey,
                     const proto::ProtoObject* const* args,
                     unsigned int argCount,
                     const proto::ProtoObject* captures) {
    // One ProtoContext per frame. Layout of automaticLocals:
    //   [0 .. arity-1]                         — params (bound at call)
    //   [arity .. arity+localCount-1]          — lets / loops
    //   [arity+localCount .. + kOperandStackSize-1]  — operand stack
    const unsigned int locBase   = 0;
    const unsigned int stackBase = static_cast<unsigned int>(mod.arity()
                                                              + mod.localCount());
    const unsigned int totalSize = stackBase + kOperandStackSize;

    proto::ProtoContext frame(parent->space, parent);
    frame.resizeAutomaticLocals(totalSize);
    unsigned int sp = 0;   // 0 means "stack empty"; max sp = kOperandStackSize

    // Bind incoming args (call site is responsible for matching arity).
    for (unsigned int i = 0; i < argCount; ++i) {
        frame.setAutomaticLocal(locBase + i, args[i]);
    }

    // Session 6 — closure captures. The wrapper passes its __captures__
    // list; we walk the BytecodeModule's captureSpecs in parallel and
    // write each value into the body-local slot it expects.
    const auto& capSpecs = mod.captureSpecs();
    if (!capSpecs.empty()) {
        if (!captures || captures == PROTO_NONE) {
            throw std::runtime_error(
                "VM: fn expects captures but none were provided");
        }
        const proto::ProtoList* clist = captures->asList(&frame);
        if (!clist || clist->getSize(&frame) != capSpecs.size()) {
            throw std::runtime_error(
                "VM: captures list size mismatch with body captureSpecs");
        }
        for (std::size_t i = 0; i < capSpecs.size(); ++i) {
            frame.setAutomaticLocal(locBase + capSpecs[i].localSlot,
                clist->getAt(&frame, static_cast<int>(i)));
        }
    }

    auto pushVal = [&](const proto::ProtoObject* v) {
        if (sp >= kOperandStackSize) {
            throw std::runtime_error("VM: operand-stack overflow");
        }
        frame.setAutomaticLocal(stackBase + sp, v);
        ++sp;
    };
    auto popVal = [&]() -> const proto::ProtoObject* {
        if (sp == 0) {
            throw std::runtime_error("VM: operand-stack underflow");
        }
        --sp;
        return frame.getAutomaticLocal(stackBase + sp);
    };
    auto peekAt = [&](unsigned int depth) -> const proto::ProtoObject* {
        // depth=0 → top, depth=1 → one below top, etc.
        return frame.getAutomaticLocal(stackBase + sp - 1 - depth);
    };

    const auto& bytes = mod.bytes();
    std::size_t pc = 0;

    while (pc + 1 < bytes.size()) {
        Op op = static_cast<Op>(bytes[pc]);
        std::uint8_t operand = bytes[pc + 1];
        pc += kInstrSize;

        switch (op) {
            case Op::NOP:
                break;

            case Op::PUSH_CONST: {
                if (operand >= mod.constCount())
                    throw std::runtime_error("VM: PUSH_CONST out-of-range");
                pushVal(materialise(&frame, mod.constAt(operand)));
                break;
            }

            case Op::PUSH_VAR: {
                if (operand >= mod.constCount())
                    throw std::runtime_error("VM: PUSH_VAR out-of-range");
                const auto& c = mod.constAt(operand);
                if (c.kind != BytecodeModule::ConstKind::Symbol)
                    throw std::runtime_error("VM: PUSH_VAR const is not a symbol");
                const proto::ProtoString* key =
                    proto::ProtoString::createSymbol(&frame, c.sval.c_str());
                const proto::ProtoObject* v =
                    globals->getAttribute(&frame, key);
                if (!v || v == PROTO_NONE) {
                    throw std::runtime_error(
                        "VM: unable to resolve symbol: " + c.sval);
                }
                pushVal(v);
                break;
            }

            case Op::PUSH_LOCAL: {
                pushVal(frame.getAutomaticLocal(locBase + operand));
                break;
            }

            case Op::STORE_LOCAL: {
                if (sp == 0)
                    throw std::runtime_error("VM: STORE_LOCAL with empty stack");
                frame.setAutomaticLocal(locBase + operand, popVal());
                break;
            }

            case Op::MAKE_FN: {
                if (operand >= mod.blockCount())
                    throw std::runtime_error("VM: MAKE_FN out-of-range");
                const BytecodeModule& subMod = mod.block(operand);

                // Session 6 — pop captureCount values from the stack and
                // collect them into a ProtoList (top-of-stack = LAST
                // capture, walking back gives source order). The compiler
                // emitted exactly captureCount PUSH_LOCALs from the
                // enclosing scope, one per capture, in capture-spec order.
                int nCaps = subMod.captureCount();
                if (sp < static_cast<unsigned int>(nCaps)) {
                    throw std::runtime_error("VM: MAKE_FN underflow on captures");
                }
                const proto::ProtoObject* capsList =
                    frame.newList()->asObject(&frame);
                if (nCaps > 0) {
                    // Walk back nCaps slots from the top to preserve emit order.
                    for (int i = 0; i < nCaps; ++i) {
                        const proto::ProtoObject* v =
                            frame.getAutomaticLocal(stackBase + sp - nCaps + i);
                        capsList = capsList->asList(&frame)
                            ->appendLast(&frame, v)->asObject(&frame);
                    }
                    sp -= static_cast<unsigned int>(nCaps);
                }

                proto::ProtoObject* wrap = const_cast<proto::ProtoObject*>(
                    fnMarkerProto->newChild(&frame, /*isMutable=*/true));
                wrap->setAttribute(&frame, bytecodeKey,
                    frame.fromLong(reinterpret_cast<long long>(&subMod)));
                wrap->setAttribute(&frame, arityKey,
                    frame.fromLong(subMod.arity()));
                wrap->setAttribute(&frame, capturesKey, capsList);
                pushVal(wrap);
                break;
            }

            case Op::CALL: {
                unsigned int argc = operand;
                if (sp < argc + 1u)
                    throw std::runtime_error("VM: CALL with insufficient stack");
                // Callable is at depth argc; args at depths argc-1..0.
                const proto::ProtoObject* callable = peekAt(argc);

                // User fn — prototype matches fnMarkerProto.
                if (callable->getPrototype(&frame) == fnMarkerProto) {
                    const proto::ProtoObject* ptrObj =
                        callable->getAttribute(&frame, bytecodeKey);
                    if (!ptrObj || !ptrObj->isInteger(&frame))
                        throw std::runtime_error("VM: malformed fn-wrapper");
                    const BytecodeModule* subMod =
                        reinterpret_cast<const BytecodeModule*>(
                            ptrObj->asLong(&frame));
                    if (subMod->arity() != static_cast<int>(argc)) {
                        throw std::runtime_error(
                            "VM: fn arity mismatch (expected " +
                            std::to_string(subMod->arity()) +
                            ", got " + std::to_string(argc) + ")");
                    }
                    // Collect args into a small C++ buffer; pop the call site
                    // values; recurse into run().
                    const proto::ProtoObject* callArgs[16];
                    if (argc > 16)
                        throw std::runtime_error("VM: >16 args not supported in v0.0.x");
                    for (unsigned int i = 0; i < argc; ++i) {
                        callArgs[i] = frame.getAutomaticLocal(
                            stackBase + sp - argc + i);
                    }
                    // Read closure captures from the wrapper (PROTO_NONE
                    // if the fn has none — body's captureSpecs is empty).
                    const proto::ProtoObject* capsVal =
                        callable->getAttribute(&frame, capturesKey);
                    sp -= (argc + 1);

                    const proto::ProtoObject* result = this->run(
                        &frame, *subMod, globals,
                        fnMarkerProto, bytecodeKey, arityKey, capturesKey,
                        callArgs, argc, capsVal);
                    pushVal(result);
                    break;
                }

                // C++ primitive — same shape as session 3/4.
                proto::ProtoContext callScope(frame.space, &frame);
                callScope.resizeAutomaticLocals(2);
                constexpr unsigned int kSlotArgs     = 0;
                constexpr unsigned int kSlotCallable = 1;
                callScope.setAutomaticLocal(kSlotArgs,
                    callScope.newList()->asObject(&callScope));
                callScope.setAutomaticLocal(kSlotCallable, callable);
                for (unsigned int i = 0; i < argc; ++i) {
                    const proto::ProtoObject* arg =
                        frame.getAutomaticLocal(stackBase + sp - argc + i);
                    const proto::ProtoList* cur =
                        callScope.getAutomaticLocal(kSlotArgs)->asList(&callScope);
                    const proto::ProtoList* updated =
                        cur->appendLast(&callScope, arg);
                    callScope.setAutomaticLocal(kSlotArgs,
                        updated->asObject(&callScope));
                }
                const proto::ProtoObject* primCallable =
                    callScope.getAutomaticLocal(kSlotCallable);
                proto::ProtoMethod fn = primCallable->asMethod(&callScope);
                if (!fn)
                    throw std::runtime_error("VM: value is not callable in CALL");
                const proto::ProtoObject* self =
                    primCallable->asMethodSelf(&callScope);
                const proto::ProtoList* argsList =
                    callScope.getAutomaticLocal(kSlotArgs)->asList(&callScope);
                const proto::ProtoObject* result =
                    fn(&callScope, self, nullptr, argsList, nullptr);
                sp -= (argc + 1);
                pushVal(result);
                break;
            }

            case Op::POP:
                if (sp == 0) throw std::runtime_error("VM: POP on empty stack");
                --sp;
                break;

            case Op::RETURN:
                if (sp == 0) return PROTO_NONE;
                return frame.getAutomaticLocal(stackBase + sp - 1);

            case Op::STORE_GLOBAL: {
                if (operand >= mod.constCount())
                    throw std::runtime_error("VM: STORE_GLOBAL out-of-range");
                const auto& c = mod.constAt(operand);
                if (c.kind != BytecodeModule::ConstKind::Symbol)
                    throw std::runtime_error("VM: STORE_GLOBAL key not a symbol");
                if (sp == 0)
                    throw std::runtime_error("VM: STORE_GLOBAL with empty stack");
                const proto::ProtoString* key =
                    proto::ProtoString::createSymbol(&frame, c.sval.c_str());
                const proto::ProtoObject* val =
                    frame.getAutomaticLocal(stackBase + sp - 1);
                const_cast<proto::ProtoObject*>(globals)
                    ->setAttribute(&frame, key, val);
                break;
            }

            case Op::JUMP:
                pc += static_cast<std::size_t>(operand) * kInstrSize;
                break;

            case Op::JUMP_IF_FALSE: {
                const proto::ProtoObject* v = popVal();
                if (v == PROTO_NONE || v == PROTO_FALSE) {
                    pc += static_cast<std::size_t>(operand) * kInstrSize;
                }
                break;
            }

            case Op::JUMP_BACK:
                pc -= static_cast<std::size_t>(operand) * kInstrSize;
                break;

            case Op::PUSH_NIL:   pushVal(PROTO_NONE);  break;
            case Op::PUSH_TRUE:  pushVal(PROTO_TRUE);  break;
            case Op::PUSH_FALSE: pushVal(PROTO_FALSE); break;

            default:
                throw std::runtime_error("VM: unknown opcode");
        }
    }
    return (sp == 0) ? PROTO_NONE : frame.getAutomaticLocal(stackBase + sp - 1);
}

} // namespace protoClojure
