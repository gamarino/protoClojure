#include "ExecutionEngine.h"
#include "BytecodeModule.h"
#include "Opcodes.h"
#include "Primitives.h"

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
        case K::Double: return ctx->fromDouble(c.dval);
        case K::String: return ctx->fromUTF8String(c.sval.c_str());
        case K::Symbol:
            return reinterpret_cast<const proto::ProtoObject*>(
                proto::ProtoString::createSymbol(ctx, c.sval.c_str()));
    }
    return PROTO_NONE;
}

} // namespace

// Helper: pick which arity-spec inside a multi-arity wrapper to use for
// a given argc. First pass: exact fixed match. Second pass: variadic with
// argc >= minArity. Returns the index into the __arities__ list, or -1.
static int pickArity(proto::ProtoContext* ctx,
                     const proto::ProtoList* aritiesList,
                     unsigned int argc) {
    unsigned long N = aritiesList->getSize(ctx);
    for (unsigned long k = 0; k < N; ++k) {
        const proto::ProtoList* spec =
            aritiesList->getAt(ctx, (int)k)->asList(ctx);
        long long a = spec->getAt(ctx, 0)->asLong(ctx);
        bool variadic = spec->getAt(ctx, 2) == PROTO_TRUE;
        if (!variadic && a == static_cast<long long>(argc)) return (int)k;
    }
    for (unsigned long k = 0; k < N; ++k) {
        const proto::ProtoList* spec =
            aritiesList->getAt(ctx, (int)k)->asList(ctx);
        long long a = spec->getAt(ctx, 0)->asLong(ctx);
        bool variadic = spec->getAt(ctx, 2) == PROTO_TRUE;
        if (variadic && static_cast<long long>(argc) >= a) return (int)k;
    }
    return -1;
}

const proto::ProtoObject*
ExecutionEngine::invoke(proto::ProtoContext* ctx,
                        const proto::ProtoObject* callable,
                        const proto::ProtoObject* const* args,
                        unsigned int argc) {
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throw std::runtime_error("VM: invoke() called outside a run()");

    // User-fn path — same shape as the in-VM dispatchCall, but the args
    // come from a caller-provided buffer rather than the operand stack.
    if (callable->getPrototype(ctx) == cc->fnMarkerProto) {
        const BytecodeModule* subMod = nullptr;
        const proto::ProtoObject* capsVal = nullptr;

        // Multi-arity? Check for __arities__ on the wrapper.
        const proto::ProtoObject* aritiesObj =
            callable->getAttribute(ctx, cc->aritiesKey);
        if (aritiesObj && aritiesObj != PROTO_NONE) {
            const proto::ProtoList* aritiesList = aritiesObj->asList(ctx);
            int k = pickArity(ctx, aritiesList, argc);
            if (k < 0)
                throw std::runtime_error(
                    "VM: invoke: no matching arity for argc=" + std::to_string(argc));
            const proto::ProtoList* spec =
                aritiesList->getAt(ctx, k)->asList(ctx);
            subMod = reinterpret_cast<const BytecodeModule*>(
                spec->getAt(ctx, 1)->asLong(ctx));
            capsVal = spec->getAt(ctx, 3);
        } else {
            const proto::ProtoObject* ptrObj =
                callable->getAttribute(ctx, cc->bytecodeKey);
            if (!ptrObj || !ptrObj->isInteger(ctx))
                throw std::runtime_error("VM: invoke: malformed fn-wrapper");
            subMod = reinterpret_cast<const BytecodeModule*>(ptrObj->asLong(ctx));
            capsVal = callable->getAttribute(ctx, cc->capturesKey);
        }

        int fixed = subMod->arity();
        bool variadic = subMod->isVariadic();
        if (!variadic && fixed != static_cast<int>(argc))
            throw std::runtime_error("VM: invoke: fn arity mismatch");
        if (variadic && static_cast<int>(argc) < fixed)
            throw std::runtime_error("VM: invoke: variadic underflow");
        const proto::ProtoObject* callArgs[17];
        unsigned int passArgc = argc;
        if (variadic) passArgc = static_cast<unsigned int>(fixed) + 1;
        for (int i = 0; i < fixed; ++i) callArgs[i] = args[i];
        if (variadic) {
            const proto::ProtoObject* rest = ctx->newList()->asObject(ctx);
            for (unsigned int i = static_cast<unsigned int>(fixed); i < argc; ++i) {
                rest = rest->asList(ctx)->appendLast(ctx, args[i])->asObject(ctx);
            }
            callArgs[fixed] = rest;
        } else {
            for (unsigned int i = static_cast<unsigned int>(fixed); i < argc; ++i) {
                callArgs[i] = args[i];
            }
        }
        return this->run(ctx, *subMod, cc->globals,
                         const_cast<proto::ProtoObject*>(cc->fnMarkerProto),
                         cc->bytecodeKey, cc->arityKey, cc->capturesKey,
                         cc->aritiesKey,
                         callArgs, passArgc, capsVal);
    }

    // C++ primitive — wrap args into a fresh ProtoList and dispatch.
    proto::ProtoContext callScope(ctx->space, ctx);
    callScope.resizeAutomaticLocals(2);
    constexpr unsigned int kSlotArgs     = 0;
    constexpr unsigned int kSlotCallable = 1;
    callScope.setAutomaticLocal(kSlotArgs,
        callScope.newList()->asObject(&callScope));
    callScope.setAutomaticLocal(kSlotCallable, callable);
    for (unsigned int i = 0; i < argc; ++i) {
        const proto::ProtoList* cur =
            callScope.getAutomaticLocal(kSlotArgs)->asList(&callScope);
        callScope.setAutomaticLocal(kSlotArgs,
            cur->appendLast(&callScope, args[i])->asObject(&callScope));
    }
    const proto::ProtoObject* primCallable =
        callScope.getAutomaticLocal(kSlotCallable);
    proto::ProtoMethod fn = primCallable->asMethod(&callScope);
    if (!fn) throw std::runtime_error("VM: invoke: value is not callable");
    const proto::ProtoObject* self = primCallable->asMethodSelf(&callScope);
    const proto::ProtoList* argsList =
        callScope.getAutomaticLocal(kSlotArgs)->asList(&callScope);
    return fn(&callScope, self, nullptr, argsList, nullptr);
}

const proto::ProtoObject*
ExecutionEngine::run(proto::ProtoContext* parent,
                     const BytecodeModule& mod,
                     const proto::ProtoObject* globals,
                     const proto::ProtoObject* fnMarkerProto,
                     const proto::ProtoString* bytecodeKey,
                     const proto::ProtoString* arityKey,
                     const proto::ProtoString* capturesKey,
                     const proto::ProtoString* aritiesKey,
                     const proto::ProtoObject* const* args,
                     unsigned int argCount,
                     const proto::ProtoObject* captures) {
    // Snapshot any prior active call context (we're nested under another
    // run() invocation, e.g. a user fn called from a primitive that
    // itself called us). Restore on every exit path so deep primitive
    // recursion stays consistent.
    const ActiveCallContext* prior = activeCallContext();
    ActiveCallContext saved = prior ? *prior : ActiveCallContext{};
    ActiveCallContext cc{this, globals,
                         fnMarkerProto, bytecodeKey, arityKey, capturesKey,
                         aritiesKey};
    setActiveCallContext(cc);
    struct Guard {
        const ActiveCallContext* prior; ActiveCallContext saved;
        ~Guard() {
            if (prior) setActiveCallContext(saved);
            else clearActiveCallContext();
        }
    } guard{prior, saved};

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

    // Dispatch a callable already on the stack at depth=argc (callable),
    // with `argc` args at depths argc-1..0. Pops callable+args, pushes
    // the result. Shared by CALL and CALL_APPLY (the latter expands its
    // args list onto the stack before calling this).
    auto dispatchCall = [&](unsigned int argc) {
        const proto::ProtoObject* callable = peekAt(argc);

        // User fn — prototype matches fnMarkerProto.
        if (callable->getPrototype(&frame) == fnMarkerProto) {
            const BytecodeModule* subMod = nullptr;
            const proto::ProtoObject* capsVal = nullptr;

            // Multi-arity? Pick the matching arity-spec first.
            const proto::ProtoObject* aritiesObj =
                callable->getAttribute(&frame, aritiesKey);
            if (aritiesObj && aritiesObj != PROTO_NONE) {
                const proto::ProtoList* aritiesList = aritiesObj->asList(&frame);
                int k = pickArity(&frame, aritiesList, argc);
                if (k < 0)
                    throw std::runtime_error(
                        "VM: no matching arity for argc=" + std::to_string(argc));
                const proto::ProtoList* spec =
                    aritiesList->getAt(&frame, k)->asList(&frame);
                subMod = reinterpret_cast<const BytecodeModule*>(
                    spec->getAt(&frame, 1)->asLong(&frame));
                capsVal = spec->getAt(&frame, 3);
            } else {
                const proto::ProtoObject* ptrObj =
                    callable->getAttribute(&frame, bytecodeKey);
                if (!ptrObj || !ptrObj->isInteger(&frame))
                    throw std::runtime_error("VM: malformed fn-wrapper");
                subMod = reinterpret_cast<const BytecodeModule*>(
                    ptrObj->asLong(&frame));
                capsVal = callable->getAttribute(&frame, capturesKey);
            }

            int fixed = subMod->arity();
            bool variadic = subMod->isVariadic();
            if (!variadic && fixed != static_cast<int>(argc)) {
                throw std::runtime_error(
                    "VM: fn arity mismatch (expected " +
                    std::to_string(fixed) +
                    ", got " + std::to_string(argc) + ")");
            }
            if (variadic && static_cast<int>(argc) < fixed) {
                throw std::runtime_error(
                    "VM: variadic fn needs at least " +
                    std::to_string(fixed) + " args, got " +
                    std::to_string(argc));
            }
            const proto::ProtoObject* callArgs[17];
            unsigned int passArgc = argc;
            if (variadic) passArgc = static_cast<unsigned int>(fixed) + 1;
            if (passArgc > 17)
                throw std::runtime_error("VM: >16 args not supported in v0.7.x");
            for (int i = 0; i < fixed; ++i) {
                callArgs[i] = frame.getAutomaticLocal(
                    stackBase + sp - argc + i);
            }
            if (variadic) {
                const proto::ProtoObject* rest =
                    frame.newList()->asObject(&frame);
                for (unsigned int i = static_cast<unsigned int>(fixed); i < argc; ++i) {
                    const proto::ProtoObject* v =
                        frame.getAutomaticLocal(stackBase + sp - argc + i);
                    rest = rest->asList(&frame)
                        ->appendLast(&frame, v)->asObject(&frame);
                }
                callArgs[fixed] = rest;
            } else {
                for (unsigned int i = static_cast<unsigned int>(fixed);
                     i < argc; ++i) {
                    callArgs[i] = frame.getAutomaticLocal(
                        stackBase + sp - argc + i);
                }
            }
            sp -= (argc + 1);
            const proto::ProtoObject* result = this->run(
                &frame, *subMod, globals,
                fnMarkerProto, bytecodeKey, arityKey, capturesKey, aritiesKey,
                callArgs, passArgc, capsVal);
            pushVal(result);
            return;
        }

        // C++ primitive.
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
                dispatchCall(argc);
                break;
            }

            case Op::CALL_APPLY: {
                // Stack: [..., callable, args-list]. Pop list, expand its
                // elements onto the stack as positional args, then dispatch
                // with argc = list size. Reuses the same dispatch as CALL.
                if (sp < 2)
                    throw std::runtime_error("VM: CALL_APPLY underflow");
                const proto::ProtoObject* listObj = popVal();
                if (!listObj)
                    throw std::runtime_error("VM: CALL_APPLY: nil args list");
                const proto::ProtoList* argsList = listObj->asList(&frame);
                if (!argsList)
                    throw std::runtime_error("VM: CALL_APPLY: not a list");
                unsigned long ln = argsList->getSize(&frame);
                for (unsigned long i = 0; i < ln; ++i) {
                    pushVal(argsList->getAt(&frame, static_cast<int>(i)));
                }
                dispatchCall(static_cast<unsigned int>(ln));
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

            case Op::JUMP_IF_TRUE: {
                const proto::ProtoObject* v = popVal();
                if (v != PROTO_NONE && v != PROTO_FALSE) {
                    pc += static_cast<std::size_t>(operand) * kInstrSize;
                }
                break;
            }

            case Op::DUP: {
                if (sp == 0) throw std::runtime_error("VM: DUP on empty stack");
                pushVal(frame.getAutomaticLocal(stackBase + sp - 1));
                break;
            }

            case Op::MAKE_FN_MULTI: {
                if (operand >= mod.arityGroupCount())
                    throw std::runtime_error("VM: MAKE_FN_MULTI out-of-range");
                const auto& group = mod.arityGroup(operand);
                // Tally captures across all arities and slice the stack
                // back into per-arity lists.
                std::size_t totalCaps = 0;
                for (std::size_t bi : group.blockIndices) {
                    totalCaps += static_cast<std::size_t>(
                        mod.block(bi).captureCount());
                }
                if (sp < totalCaps)
                    throw std::runtime_error("VM: MAKE_FN_MULTI underflow");

                // Build the __arities__ list of 4-tuples
                //   (arity, bc-ptr, variadic?, captures-list)
                const proto::ProtoObject* aritiesList =
                    frame.newList()->asObject(&frame);
                unsigned int baseSp = sp - static_cast<unsigned int>(totalCaps);
                unsigned int cur = baseSp;
                for (std::size_t bi : group.blockIndices) {
                    const BytecodeModule& sub = mod.block(bi);
                    int nCaps = sub.captureCount();
                    const proto::ProtoObject* capsList =
                        frame.newList()->asObject(&frame);
                    for (int i = 0; i < nCaps; ++i) {
                        const proto::ProtoObject* v =
                            frame.getAutomaticLocal(stackBase + cur + i);
                        capsList = capsList->asList(&frame)
                            ->appendLast(&frame, v)->asObject(&frame);
                    }
                    cur += static_cast<unsigned int>(nCaps);

                    // spec = (arity, bc-ptr, variadic, caps)
                    const proto::ProtoObject* spec =
                        frame.newList()->asObject(&frame);
                    spec = spec->asList(&frame)
                        ->appendLast(&frame, frame.fromLong(sub.arity()))
                        ->asObject(&frame);
                    spec = spec->asList(&frame)
                        ->appendLast(&frame, frame.fromLong(
                            reinterpret_cast<long long>(&sub)))
                        ->asObject(&frame);
                    spec = spec->asList(&frame)
                        ->appendLast(&frame,
                            sub.isVariadic() ? PROTO_TRUE : PROTO_FALSE)
                        ->asObject(&frame);
                    spec = spec->asList(&frame)
                        ->appendLast(&frame, capsList)
                        ->asObject(&frame);

                    aritiesList = aritiesList->asList(&frame)
                        ->appendLast(&frame, spec)
                        ->asObject(&frame);
                }
                sp = baseSp;

                proto::ProtoObject* wrap = const_cast<proto::ProtoObject*>(
                    fnMarkerProto->newChild(&frame, /*isMutable=*/true));
                wrap->setAttribute(&frame, aritiesKey, aritiesList);
                pushVal(wrap);
                break;
            }

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
