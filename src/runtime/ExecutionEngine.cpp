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

// SmallInt tagged pointer constants — mirror of protoCore's
// PROTO_SMALL_INT_TAG_MASK/VALUE for the hot-path checks. Re-declared here
// to dodge the per-call function-call cost of going through the inline
// helper; both definitions must match headers/protoCore.h.
constexpr unsigned long kSmallIntTagMask  = 0x3FFUL;
constexpr unsigned long kSmallIntTagValue = 0x001UL;

inline bool isSmallIntFast(const proto::ProtoObject* v) {
    return (reinterpret_cast<unsigned long>(v) & kSmallIntTagMask) ==
           kSmallIntTagValue;
}

inline long long smallIntValueFast(const proto::ProtoObject* v) {
    // Inverse of fromLong's `(v << 10) | tag` — arithmetic right-shift
    // preserves sign for negatives, since the tag bits are always zero.
    return reinterpret_cast<long long>(v) >> 10;
}

inline const proto::ProtoObject* smallIntFromLong(long long v) {
    return reinterpret_cast<const proto::ProtoObject*>(
        (v << 10) | static_cast<long long>(kSmallIntTagValue));
}

inline bool smallIntFitsLong(long long v) {
    // Same bounds as PROTO_SMALL_INT_MIN/MAX (53-bit).
    constexpr long long kMax =  (1LL << 53) - 1;
    constexpr long long kMin = -(1LL << 53);
    return v >= kMin && v <= kMax;
}

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

// Session 13 — for a kw-based callee, extract the declared kwKey values
// from the trailing map (if any) and write them into `out` in kwKey
// order. Returns true if a trailing map was consumed (the caller must
// then drop the last arg from the positional list); false if no map
// was supplied and the slots stay nil.
static bool extractKwVals(proto::ProtoContext* ctx,
                          const BytecodeModule* subMod,
                          const proto::ProtoObject* maybeMap,
                          const proto::ProtoObject* mapMarkerProto,
                          const proto::ProtoString* entriesKey,
                          const proto::ProtoObject** out) {
    const auto& kkeys = subMod->kwKeys();
    if (!maybeMap || maybeMap == PROTO_NONE) {
        for (std::size_t i = 0; i < kkeys.size(); ++i) out[i] = PROTO_NONE;
        return false;
    }
    if (maybeMap->getPrototype(ctx) != mapMarkerProto) {
        for (std::size_t i = 0; i < kkeys.size(); ++i) out[i] = PROTO_NONE;
        return false;
    }
    const proto::ProtoObject* eRaw = maybeMap->getAttribute(ctx, entriesKey);
    const proto::ProtoSparseList* sparse = eRaw
        ? reinterpret_cast<const proto::ProtoSparseList*>(eRaw)
        : ctx->newSparseList();
    for (std::size_t i = 0; i < kkeys.size(); ++i) {
        // Build the keyword key (`:name`) and probe the sparse map by
        // hash + bucket linear scan, same shape as prim_get.
        std::string kw = ":" + kkeys[i].name;
        const proto::ProtoString* sym =
            proto::ProtoString::createSymbol(ctx, kw.c_str());
        const proto::ProtoObject* symObj =
            reinterpret_cast<const proto::ProtoObject*>(sym);
        unsigned long h = symObj->getHash(ctx);
        out[i] = PROTO_NONE;
        if (!sparse->has(ctx, h)) continue;
        const proto::ProtoObject* b = sparse->getAt(ctx, h);
        if (!b) continue;
        const proto::ProtoList* bucket = b->asList(ctx);
        unsigned long bn = bucket->getSize(ctx);
        for (unsigned long j = 0; j < bn; j += 2) {
            const proto::ProtoObject* k = bucket->getAt(ctx, (int)j);
            if (k->compare(ctx, symObj) == 0) {
                out[i] = bucket->getAt(ctx, (int)(j + 1));
                break;
            }
        }
    }
    return true;
}

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

    // Session 12 — single getPrototype probe picks the path. Single-
    // arity wrappers cost ONE getAttribute (bytecode); the captures
    // read is skipped entirely when the body has no captures (the
    // dominant case for top-level defns like the bench fib).
    const proto::ProtoObject* proto = callable->getPrototype(ctx);
    const bool isSingle = (proto == cc->fnSingleProto);
    const bool isMulti  = (proto == cc->fnMultiProto);

    if (isSingle || isMulti) {
        const BytecodeModule* subMod = nullptr;
        const proto::ProtoObject* capsVal = nullptr;

        if (isMulti) {
            const proto::ProtoObject* aritiesObj =
                callable->getAttribute(ctx, cc->aritiesKey);
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
            // C optimisation — only fetch captures when the body needs them.
            if (subMod->captureCount() > 0) {
                capsVal = callable->getAttribute(ctx, cc->capturesKey);
            }
        }

        int fixed = subMod->arity();
        bool variadic = subMod->isVariadic();
        bool kwBased  = subMod->isKwBased();

        // kw-based callee: the optional last positional argument is the
        // kwArgs map. Consume it before arity-checking.
        const proto::ProtoObject* kwArgsMap = nullptr;
        unsigned int posArgc = argc;
        if (kwBased) {
            if (static_cast<int>(posArgc) == fixed + 1) {
                kwArgsMap = args[posArgc - 1];
                posArgc -= 1;
            } else if (static_cast<int>(posArgc) != fixed) {
                throw std::runtime_error(
                    "VM: invoke: kw-based fn expects " + std::to_string(fixed) +
                    " positionals (+ optional trailing map), got " +
                    std::to_string(argc));
            }
        } else {
            if (!variadic && fixed != static_cast<int>(posArgc))
                throw std::runtime_error("VM: invoke: fn arity mismatch");
            if (variadic && static_cast<int>(posArgc) < fixed)
                throw std::runtime_error("VM: invoke: variadic underflow");
        }
        const proto::ProtoObject* callArgs[17];
        unsigned int passArgc = posArgc;
        if (variadic) passArgc = static_cast<unsigned int>(fixed) + 1;
        for (int i = 0; i < fixed; ++i) callArgs[i] = args[i];
        if (variadic) {
            const proto::ProtoObject* rest = ctx->newList()->asObject(ctx);
            for (unsigned int i = static_cast<unsigned int>(fixed); i < posArgc; ++i) {
                rest = rest->asList(ctx)->appendLast(ctx, args[i])->asObject(ctx);
            }
            callArgs[fixed] = rest;
        } else if (!kwBased) {
            for (unsigned int i = static_cast<unsigned int>(fixed); i < posArgc; ++i) {
                callArgs[i] = args[i];
            }
        }

        const proto::ProtoObject* kwVals[16];
        unsigned int kwCount = 0;
        if (kwBased) {
            kwCount = static_cast<unsigned int>(subMod->kwKeys().size());
            if (kwCount > 16)
                throw std::runtime_error("VM: invoke: >16 kw-args not supported in v0.13");
            extractKwVals(ctx, subMod, kwArgsMap,
                          cc->mapMarkerProto, cc->entriesKey, kwVals);
        }

        return this->run(ctx, *subMod, cc->globals,
                         const_cast<proto::ProtoObject*>(cc->fnSingleProto),
                         const_cast<proto::ProtoObject*>(cc->fnMultiProto),
                         const_cast<proto::ProtoObject*>(cc->mapMarkerProto),
                         const_cast<proto::ProtoObject*>(cc->atomMarkerProto),
                         const_cast<proto::ProtoObject*>(cc->futureMarkerProto),
                         const_cast<proto::ProtoObject*>(cc->promiseMarkerProto),
                         const_cast<proto::ProtoObject*>(cc->actorMarkerProto),
                         cc->bytecodeKey, cc->arityKey, cc->capturesKey,
                         cc->aritiesKey, cc->entriesKey, cc->valueKey,
                         cc->watchesKey,
                         cc->thunkKey, cc->ccBlobKey, cc->threadKey,
                         cc->resultKey, cc->doneKey,
                         cc->actorStateKey,
                         callArgs, passArgc, capsVal,
                         kwBased ? kwVals : nullptr, kwCount,
                         kwBased ? kwArgsMap : nullptr);
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
                     const proto::ProtoObject* fnSingleProto,
                     const proto::ProtoObject* fnMultiProto,
                     const proto::ProtoObject* mapMarkerProto,
                     const proto::ProtoObject* atomMarkerProto,
                     const proto::ProtoObject* futureMarkerProto,
                     const proto::ProtoObject* promiseMarkerProto,
                     const proto::ProtoObject* actorMarkerProto,
                     const proto::ProtoString* bytecodeKey,
                     const proto::ProtoString* arityKey,
                     const proto::ProtoString* capturesKey,
                     const proto::ProtoString* aritiesKey,
                     const proto::ProtoString* entriesKey,
                     const proto::ProtoString* valueKey,
                     const proto::ProtoString* watchesKey,
                     const proto::ProtoString* thunkKey,
                     const proto::ProtoString* ccBlobKey,
                     const proto::ProtoString* threadKey,
                     const proto::ProtoString* resultKey,
                     const proto::ProtoString* doneKey,
                     const proto::ProtoString* actorStateKey,
                     const proto::ProtoObject* const* args,
                     unsigned int argCount,
                     const proto::ProtoObject* captures,
                     const proto::ProtoObject* const* kwVals,
                     unsigned int kwCount,
                     const proto::ProtoObject* kwMap) {
    // Snapshot any prior active call context (we're nested under another
    // run() invocation, e.g. a user fn called from a primitive that
    // itself called us). Restore on every exit path so deep primitive
    // recursion stays consistent.
    const ActiveCallContext* prior = activeCallContext();
    ActiveCallContext saved = prior ? *prior : ActiveCallContext{};
    ActiveCallContext cc{this, globals,
                         fnSingleProto, fnMultiProto, mapMarkerProto,
                         atomMarkerProto, futureMarkerProto, promiseMarkerProto,
                         actorMarkerProto,
                         bytecodeKey, arityKey, capturesKey, aritiesKey,
                         entriesKey, valueKey, watchesKey,
                         thunkKey, ccBlobKey, threadKey, resultKey, doneKey,
                         actorStateKey};
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

    // Session 13 — kw-based fn: populate the declared :keys slots from
    // the caller-supplied kwVals array (already extracted from the
    // trailing map at dispatch time). Missing keys → slot stays nil.
    // Session 14 — also write the raw kwMap into the `:as` binding's
    // slot when one was declared.
    if (mod.isKwBased()) {
        const auto& kkeys = mod.kwKeys();
        for (std::size_t i = 0; i < kkeys.size(); ++i) {
            const proto::ProtoObject* v =
                (kwVals && i < kwCount) ? kwVals[i] : PROTO_NONE;
            frame.setAutomaticLocal(locBase + kkeys[i].localSlot, v);
        }
        if (mod.asSlot() >= 0) {
            frame.setAutomaticLocal(locBase + mod.asSlot(),
                kwMap ? kwMap : PROTO_NONE);
        }
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
        const proto::ProtoObject* proto = callable->getPrototype(&frame);
        const bool isSingle = (proto == fnSingleProto);
        const bool isMulti  = (proto == fnMultiProto);

        if (isSingle || isMulti) {
            const BytecodeModule* subMod = nullptr;
            const proto::ProtoObject* capsVal = nullptr;

            if (isMulti) {
                // Multi-arity — read __arities__ and pick the spec.
                const proto::ProtoObject* aritiesObj =
                    callable->getAttribute(&frame, aritiesKey);
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
                // Single-arity — one getAttribute (bytecode). The
                // captures lookup is skipped when the body has no
                // captures (the dominant case for top-level defns:
                // fib, sum-loop, factorial, ...).
                const proto::ProtoObject* ptrObj =
                    callable->getAttribute(&frame, bytecodeKey);
                if (!ptrObj || !ptrObj->isInteger(&frame))
                    throw std::runtime_error("VM: malformed fn-wrapper");
                subMod = reinterpret_cast<const BytecodeModule*>(
                    ptrObj->asLong(&frame));
                if (subMod->captureCount() > 0) {
                    capsVal = callable->getAttribute(&frame, capturesKey);
                }
            }

            int fixed = subMod->arity();
            bool variadic = subMod->isVariadic();
            bool kwBased  = subMod->isKwBased();

            const proto::ProtoObject* kwArgsMap = nullptr;
            unsigned int posArgc = argc;
            if (kwBased) {
                if (static_cast<int>(posArgc) == fixed + 1) {
                    kwArgsMap = frame.getAutomaticLocal(
                        stackBase + sp - 1);
                    posArgc -= 1;
                } else if (static_cast<int>(posArgc) != fixed) {
                    throw std::runtime_error(
                        "VM: kw-based fn expects " + std::to_string(fixed) +
                        " positionals (+ optional trailing map), got " +
                        std::to_string(argc));
                }
            } else {
                if (!variadic && fixed != static_cast<int>(posArgc)) {
                    throw std::runtime_error(
                        "VM: fn arity mismatch (expected " +
                        std::to_string(fixed) +
                        ", got " + std::to_string(argc) + ")");
                }
                if (variadic && static_cast<int>(posArgc) < fixed) {
                    throw std::runtime_error(
                        "VM: variadic fn needs at least " +
                        std::to_string(fixed) + " args, got " +
                        std::to_string(argc));
                }
            }
            const proto::ProtoObject* callArgs[17];
            unsigned int passArgc = posArgc;
            if (variadic) passArgc = static_cast<unsigned int>(fixed) + 1;
            if (passArgc > 17)
                throw std::runtime_error("VM: >16 args not supported in v0.13");
            for (int i = 0; i < fixed; ++i) {
                callArgs[i] = frame.getAutomaticLocal(
                    stackBase + sp - argc + i);
            }
            if (variadic) {
                const proto::ProtoObject* rest =
                    frame.newList()->asObject(&frame);
                for (unsigned int i = static_cast<unsigned int>(fixed); i < posArgc; ++i) {
                    const proto::ProtoObject* v =
                        frame.getAutomaticLocal(stackBase + sp - argc + i);
                    rest = rest->asList(&frame)
                        ->appendLast(&frame, v)->asObject(&frame);
                }
                callArgs[fixed] = rest;
            } else if (!kwBased) {
                for (unsigned int i = static_cast<unsigned int>(fixed);
                     i < posArgc; ++i) {
                    callArgs[i] = frame.getAutomaticLocal(
                        stackBase + sp - argc + i);
                }
            }

            const proto::ProtoObject* kwVals[16];
            unsigned int kwCount = 0;
            if (kwBased) {
                kwCount = static_cast<unsigned int>(subMod->kwKeys().size());
                if (kwCount > 16)
                    throw std::runtime_error("VM: >16 kw-args not supported in v0.13");
                extractKwVals(&frame, subMod, kwArgsMap,
                              mapMarkerProto, entriesKey, kwVals);
            }

            sp -= (argc + 1);
            const proto::ProtoObject* result = this->run(
                &frame, *subMod, globals,
                fnSingleProto, fnMultiProto, mapMarkerProto,
                atomMarkerProto, futureMarkerProto, promiseMarkerProto,
                actorMarkerProto,
                bytecodeKey, arityKey, capturesKey, aritiesKey,
                entriesKey, valueKey, watchesKey,
                thunkKey, ccBlobKey, threadKey, resultKey, doneKey,
                actorStateKey,
                callArgs, passArgc, capsVal,
                kwBased ? kwVals : nullptr, kwCount,
                kwBased ? kwArgsMap : nullptr);
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

                // Single-arity wrapper — fnSingleProto carries
                // __bytecode__ always and __captures__ only when the
                // body has captures. arityKey was vestigial (the
                // dispatcher reads arity directly from subMod) and is
                // no longer set on the wrapper.
                proto::ProtoObject* wrap = const_cast<proto::ProtoObject*>(
                    fnSingleProto->newChild(&frame, /*isMutable=*/true));
                wrap->setAttribute(&frame, bytecodeKey,
                    frame.fromLong(reinterpret_cast<long long>(&subMod)));
                if (nCaps > 0) {
                    wrap->setAttribute(&frame, capturesKey, capsList);
                }
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

            case Op::CALL_KW: {
                // Stack: [..., callable, pos1, ..., posK, kwMap].
                // operand = K + 1 (positionals counting the kwMap).
                // If callee is a user fn declaring isKwBased, keep the
                // map and dispatch (session 13 path). Otherwise unpack
                // the kwMap into k,v,k,v positionals so the call sees
                // the literal `:k v` shape the source had — backwards
                // compatibility for primitives + non-kw user fns.
                unsigned int argc = operand;
                if (sp < argc + 1u)
                    throw std::runtime_error("VM: CALL_KW with insufficient stack");
                const proto::ProtoObject* callable = peekAt(argc);
                bool calleeIsKwBased = false;
                if (callable->getPrototype(&frame) == fnSingleProto) {
                    const proto::ProtoObject* ptrObj =
                        callable->getAttribute(&frame, bytecodeKey);
                    if (ptrObj && ptrObj->isInteger(&frame)) {
                        const BytecodeModule* subMod =
                            reinterpret_cast<const BytecodeModule*>(
                                ptrObj->asLong(&frame));
                        calleeIsKwBased = subMod->isKwBased();
                    }
                }
                if (calleeIsKwBased) {
                    dispatchCall(argc);
                } else {
                    // Pop the kwMap, walk all its (k,v) pairs into a
                    // transient ProtoList, then push them as alternating
                    // positionals and dispatch. The flat list lets us
                    // count without coupling pushVal to the C callback.
                    const proto::ProtoObject* kwMap = popVal();
                    if (!kwMap || kwMap->getPrototype(&frame) != mapMarkerProto)
                        throw std::runtime_error("VM: CALL_KW: kwMap not a map");
                    const proto::ProtoObject* eRaw =
                        kwMap->getAttribute(&frame, entriesKey);
                    const proto::ProtoSparseList* sparse = eRaw
                        ? reinterpret_cast<const proto::ProtoSparseList*>(eRaw)
                        : frame.newSparseList();
                    proto::ProtoContext tmp(frame.space, &frame);
                    tmp.resizeAutomaticLocals(1);
                    tmp.setAutomaticLocal(0, tmp.newList()->asObject(&tmp));
                    struct Acc { proto::ProtoContext* tmp; } acc{&tmp};
                    sparse->processElements(&tmp, &acc,
                        [](proto::ProtoContext* /*c*/, void* self,
                           unsigned long /*hash*/,
                           const proto::ProtoObject* bucketObj) {
                            auto* a = static_cast<Acc*>(self);
                            if (!bucketObj) return;
                            const proto::ProtoList* bucket = bucketObj->asList(a->tmp);
                            const proto::ProtoObject* lst = a->tmp->getAutomaticLocal(0);
                            unsigned long n = bucket->getSize(a->tmp);
                            for (unsigned long i = 0; i < n; ++i) {
                                lst = lst->asList(a->tmp)
                                    ->appendLast(a->tmp, bucket->getAt(a->tmp, (int)i))
                                    ->asObject(a->tmp);
                            }
                            a->tmp->setAutomaticLocal(0, lst);
                        });
                    const proto::ProtoList* flat =
                        tmp.getAutomaticLocal(0)->asList(&tmp);
                    unsigned long flatN = flat->getSize(&tmp);
                    for (unsigned long i = 0; i < flatN; ++i) {
                        pushVal(flat->getAt(&tmp, (int)i));
                    }
                    dispatchCall(argc - 1 + static_cast<unsigned int>(flatN));
                }
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
                    fnMultiProto->newChild(&frame, /*isMutable=*/true));
                wrap->setAttribute(&frame, aritiesKey, aritiesList);
                pushVal(wrap);
                break;
            }

            case Op::PUSH_NIL:   pushVal(PROTO_NONE);  break;
            case Op::PUSH_TRUE:  pushVal(PROTO_TRUE);  break;
            case Op::PUSH_FALSE: pushVal(PROTO_FALSE); break;

            // Session 11 — SmallInt fast-path binary ops. On the hot path
            // (both args tagged SmallInt, result fits) we inline the
            // arithmetic and skip the ProtoMethod indirection entirely.
            // On any other shape we fall back to looking up the
            // corresponding primitive on the globals namespace and
            // dispatching it as a regular CALL — same semantics as if
            // the compiler had emitted PUSH_VAR + CALL.
            case Op::ADD:
            case Op::SUB:
            case Op::MUL:
            case Op::LT:
            case Op::LE:
            case Op::GT:
            case Op::GE:
            case Op::EQ: {
                if (sp < 2) throw std::runtime_error("VM: binop underflow");
                const proto::ProtoObject* a =
                    frame.getAutomaticLocal(stackBase + sp - 2);
                const proto::ProtoObject* b =
                    frame.getAutomaticLocal(stackBase + sp - 1);

                // Fast path: both operands tagged SmallInt. Inline the
                // arithmetic; on overflow past the 53-bit SmallInt range
                // fall through to protoCore's promoting `add`/`subtract`/
                // `multiply`, which materialise a LargeInteger when
                // needed (the "infinite-precision" path documented in
                // headers/protoCore.h).
                if (isSmallIntFast(a) && isSmallIntFast(b)) {
                    long long av = smallIntValueFast(a);
                    long long bv = smallIntValueFast(b);
                    sp -= 2;
                    const proto::ProtoObject* r = PROTO_NONE;
                    switch (op) {
                        case Op::ADD: {
                            long long s = av + bv;
                            if (smallIntFitsLong(s)) r = smallIntFromLong(s);
                            else                     r = a->add(&frame, b);
                            break;
                        }
                        case Op::SUB: {
                            long long s = av - bv;
                            if (smallIntFitsLong(s)) r = smallIntFromLong(s);
                            else                     r = a->subtract(&frame, b);
                            break;
                        }
                        case Op::MUL: {
                            // Multiplication can overflow long long itself
                            // for large SmallInts; trust protoCore's slow
                            // path when either operand exceeds the safe
                            // 26-bit half-range, OR when the inline
                            // result no longer fits SmallInt.
                            constexpr long long kHalf = 1LL << 26;
                            if (av <= -kHalf || av >= kHalf ||
                                bv <= -kHalf || bv >= kHalf) {
                                r = a->multiply(&frame, b);
                            } else {
                                long long s = av * bv;
                                if (smallIntFitsLong(s)) r = smallIntFromLong(s);
                                else                     r = a->multiply(&frame, b);
                            }
                            break;
                        }
                        case Op::LT: r = (av <  bv) ? PROTO_TRUE : PROTO_FALSE; break;
                        case Op::LE: r = (av <= bv) ? PROTO_TRUE : PROTO_FALSE; break;
                        case Op::GT: r = (av >  bv) ? PROTO_TRUE : PROTO_FALSE; break;
                        case Op::GE: r = (av >= bv) ? PROTO_TRUE : PROTO_FALSE; break;
                        case Op::EQ: r = (av == bv) ? PROTO_TRUE : PROTO_FALSE; break;
                        default: break;
                    }
                    pushVal(r);
                    break;
                }

                // Slow path: at least one operand is NOT a tagged
                // SmallInt — could be Float, LargeInteger, or a non-
                // numeric mistake. Route through protoCore's promoting
                // add/subtract/multiply/compare which handle SmallInt ↔
                // LargeInteger ↔ Float automatically. Throws on non-
                // numeric input. The compiler never emits these opcodes
                // when the operator is shadowed by a local, so we don't
                // need to honour user-shadowing here.
                sp -= 2;
                const proto::ProtoObject* r = PROTO_NONE;
                switch (op) {
                    case Op::ADD: r = a->add(&frame, b);      break;
                    case Op::SUB: r = a->subtract(&frame, b); break;
                    case Op::MUL: r = a->multiply(&frame, b); break;
                    case Op::LT:  r = a->compare(&frame, b) <  0 ? PROTO_TRUE : PROTO_FALSE; break;
                    case Op::LE:  r = a->compare(&frame, b) <= 0 ? PROTO_TRUE : PROTO_FALSE; break;
                    case Op::GT:  r = a->compare(&frame, b) >  0 ? PROTO_TRUE : PROTO_FALSE; break;
                    case Op::GE:  r = a->compare(&frame, b) >= 0 ? PROTO_TRUE : PROTO_FALSE; break;
                    case Op::EQ:  r = a->compare(&frame, b) == 0 ? PROTO_TRUE : PROTO_FALSE; break;
                    default: break;
                }
                pushVal(r);
                break;
            }

            default:
                throw std::runtime_error("VM: unknown opcode");
        }
    }
    return (sp == 0) ? PROTO_NONE : frame.getAutomaticLocal(stackBase + sp - 1);
}

} // namespace protoClojure
