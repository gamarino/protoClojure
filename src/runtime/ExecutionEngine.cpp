#include "ExecutionEngine.h"
#include "Utf8String.h"
#include "BytecodeModule.h"
#include "CallerSlots.h"
#include "DebugHook.h"
#include "Exceptions.h"
#include "ListBuilder.h"
#include "MapOps.h"
#include "SeqTypes.h"
#include "SetOps.h"
#include "Named.h"
#include "Opcodes.h"
#include "Primitives.h"
#include "VectorOps.h"
#include "StackGuard.h"

#include "protoCore.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <vector>

// Inlining directives. GCC and Clang read the gnu:: attributes below; MSVC
// ignores them, so these macros give it the same instruction (and expand to
// nothing elsewhere; PROTOCLJ_INLINE is plain `inline` and the lambda one is
// GCC's own spelling).
#if defined(_MSC_VER) && !defined(__clang__)
#define PROTOCLJ_MSVC_NOINLINE     __declspec(noinline)
#define PROTOCLJ_INLINE            __forceinline
#define PROTOCLJ_LAMBDA_ALWAYS_INLINE [[msvc::forceinline]]
#else
#define PROTOCLJ_MSVC_NOINLINE
#define PROTOCLJ_INLINE            inline
#define PROTOCLJ_LAMBDA_ALWAYS_INLINE __attribute__((always_inline))
#endif

namespace protoClojure {

namespace {

// A frame's locals (params + let-bindings) live in slots
// 0..(arity + localCount - 1); its operand stack starts at offset
// arity + localCount with the capacity the compiled body needs
// (BytecodeModule::maxStack); `apply` reserves room for the list it spreads.

// The most parameters one call binds (a variadic fn binds its rest
// arguments as one).
constexpr unsigned int kMaxBoundParameters = 17;

// SmallInt tagged pointer constants — mirror of protoCore's
// PROTO_SMALL_INT_TAG_MASK/VALUE for the hot-path checks. Re-declared here
// to dodge the per-call function-call cost of going through the inline
// helper; both definitions must match headers/protoCore.h.
constexpr proto::proto_ulong kSmallIntTagMask  = PROTO_UL(0x3FF);
constexpr proto::proto_ulong kSmallIntTagValue = PROTO_UL(0x001);

inline bool isSmallIntFast(const proto::ProtoObject* v) {
    return (reinterpret_cast<proto::proto_ulong>(v) & kSmallIntTagMask) ==
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

// The operator a binary arithmetic or comparison opcode compiles, for error
// messages.
const char* binaryOpName(Op op) {
    switch (op) {
        case Op::ADD: return "+";
        case Op::SUB: return "-";
        case Op::MUL: return "*";
        case Op::LT:  return "<";
        case Op::LE:  return "<=";
        case Op::GT:  return ">";
        case Op::GE:  return ">=";
        case Op::EQ:  return "=";
        default:      return "operator";
    }
}

// The operators the compiler inlines as opcodes (ADD .. EQ), by opcode.
constexpr const char* kCoreOperatorNames[8] = {"+", "-", "*", "<", "<=", ">", ">=", "="};

// One bit per inlined operator (bit op - ADD): set while the global of that
// name holds something other than the built-in. STORE_GLOBAL keeps it; an
// operator opcode whose bit is set calls the global instead of computing
// (so `(def + -)` is honoured by code compiled before it). Process-wide and
// only ever conservative: a stale bit costs a global lookup, never a wrong
// answer.
std::atomic<unsigned int> g_coreOperatorRedefined{0};

// The global an operator opcode must call instead of computing (its bit is
// set and the form did not name clojure.core's), or nullptr. Out of line: it
// runs only in a program that redefined an operator.
[[gnu::noinline, gnu::cold]] PROTOCLJ_MSVC_NOINLINE
const proto::ProtoObject* redefinedOperator(proto::ProtoContext* ctx,
                                            const proto::ProtoObject* globals,
                                            Op op, std::uint32_t operand) {
    const unsigned int bit = static_cast<unsigned int>(op) - static_cast<unsigned int>(Op::ADD);
    if ((operand & 1) != 0 ||
        (g_coreOperatorRedefined.load(std::memory_order_relaxed) & (1u << bit)) == 0)
        return nullptr;
    const proto::ProtoObject* f = globals->getOwnAttributeDirect(
        ctx, proto::ProtoString::createSymbol(ctx, kCoreOperatorNames[bit]));
    if (!f) throwError(ctx, std::string("VM: unable to resolve symbol: ") + kCoreOperatorNames[bit]);
    return f;
}

// STORE_GLOBAL of `name` = `val`: updates the operator's bit.
void noteGlobalStore(proto::ProtoContext* ctx, const proto::ProtoObject* globals,
                     const std::string& name, const proto::ProtoObject* val) {
    for (unsigned int i = 0; i < 8; ++i) {
        if (name != kCoreOperatorNames[i]) continue;
        const proto::ProtoObject* core = globals->getOwnAttributeDirect(
            ctx, proto::ProtoString::createSymbol(ctx, ("clojure.core/" + name).c_str()));
        if (val != core) g_coreOperatorRedefined.fetch_or(1u << i, std::memory_order_relaxed);
        else             g_coreOperatorRedefined.fetch_and(~(1u << i), std::memory_order_relaxed);
        return;
    }
}

// protoCore's pointer tag of a method value (POINTER_TAG_METHOD,
// headers/proto_internal.h): every primitive.
constexpr proto::proto_ulong kTagMethod = 12;

inline bool smallIntFitsLong(long long v) {
    // Same bounds as PROTO_SMALL_INT_MIN/MAX (53-bit).
    constexpr long long kMax =  (1LL << 53) - 1;
    constexpr long long kMin = -(1LL << 53);
    return v >= kMin && v <= kMax;
}

// A BigInteger constant rebuilt from its digits. Out of line: it is rare, and
// keeping it out of materialise leaves the PUSH_CONST path of the common
// kinds as compact as before.
[[gnu::noinline, gnu::cold]] PROTOCLJ_MSVC_NOINLINE
const proto::ProtoObject* materialiseBigInteger(proto::ProtoContext* ctx,
                                                const BytecodeModule::Const& c) {
    return ctx->fromString(c.sval.c_str(), 10);
}

// The value a PUSH_CONST pushes. Keyword literals and quoted symbols are
// Named constants holding the value the compiler interned (Named.h); quoted
// lists, vectors and maps are Datum constants holding the value the reader
// yielded, pinned by the module.
const proto::ProtoObject* materialise(proto::ProtoContext* ctx,
                                       const BytecodeModule::Const& c) {
    using K = BytecodeModule::ConstKind;
    switch (c.kind) {
        case K::Long:   return ctx->fromLong(c.ival);
        case K::Double: return ctx->fromDouble(c.dval);
        case K::String:
            // A literal holding a NUL byte (`"a\u0000b"`) needs the
            // length-taking constructor; the common case keeps the old one.
            if (c.sval.find('\0') != std::string::npos) return makeUtf8String(ctx, c.sval);
            return ctx->fromUTF8String(c.sval.c_str());
        case K::Named:  return c.named;
        case K::Datum:  return c.named;
        case K::BigInteger: return materialiseBigInteger(ctx, c);
        case K::Symbol: break;  // a global name, never a value
    }
    throwError(ctx, "VM: PUSH_CONST of a global name: " + c.sval);
}

// A keyword or quoted symbol `k` called as `(k coll)` / `(k coll not-found)`,
// or a map `m` called as `(m key)` / `(m key not-found)`: a map lookup with
// the semantics of `get`. The result is the value under the key, or
// `not-found` (nil by default) when the key is absent or, for a keyword,
// when `coll` is not a map. `callableIsMap` tells the two shapes apart; the
// caller has checked that `callable` is a named value or a map.
//
// Cost: one mapGet, which hashes the key (MapOps.h); `callable` and
// `args` must be rooted by the caller.
const proto::ProtoObject* callLookup(proto::ProtoContext* ctx,
                                     const NamedLayout& named,
                                     const proto::ProtoObject* callable,
                                     bool callableIsMap,
                                     const proto::ProtoObject* const* args,
                                     unsigned int argc) {
    if (argc != 1 && argc != 2) {
        std::string what = isSet(ctx, callable) ? "a set" : "a map";
        if (!callableIsMap) {
            const std::string spelling =
                namedSpelling(ctx, named, callable)->toStdString(ctx);
            what = (spelling[0] == ':' ? "keyword " : "symbol ") + spelling;
        }
        throwError(ctx, 
            "VM: " + what + " called as a function expects 1 or 2 arguments, got " +
            std::to_string(argc));
    }
    const proto::ProtoObject* m   = callableIsMap ? callable : args[0];
    const proto::ProtoObject* key = callableIsMap ? args[0] : callable;
    const proto::ProtoObject* notFound = (argc == 2) ? args[1] : PROTO_NONE;
    bool found = false;
    // A set is a function of its elements, and a keyword looks itself up in
    // a set, as `get` does (SetOps.h).
    if (isSet(ctx, m)) {
        const proto::ProtoObject* e = setGet(ctx, m, key, &found);
        return found ? e : notFound;
    }
    if (!callableIsMap && !isMap(m)) return notFound;
    const proto::ProtoObject* v = mapGet(ctx, m, key, &found);
    return found ? v : notFound;
}

// A vector called as a function, `([10 20 30] 1)`: the element at that
// index, as `nth` without a default. As in Clojure it takes exactly one
// integer argument, and an index out of range raises
// IndexOutOfBoundsException. Cold: only reached when the callable is not a
// function.
[[gnu::cold]] const proto::ProtoObject* callVector(proto::ProtoContext* ctx,
                                                   const proto::ProtoObject* v,
                                                   const proto::ProtoObject* const* args,
                                                   unsigned int argc) {
    if (argc != 1)
        throwClassed(ctx, "ArityException",
                     "Wrong number of args (" + std::to_string(argc) + ") passed to: a vector");
    const proto::ProtoObject* i = args[0];
    if (!i || i == PROTO_NONE || !i->isInteger(ctx))
        throwClassed(ctx, "IllegalArgumentException", "Key must be integer");
    const proto::ProtoList* items = vectorItems(ctx, v);
    const long long n = static_cast<long long>(items->getSize(ctx));
    const long long k = i->asLong(ctx);
    if (k < 0 || k >= n) throwClassed(ctx, "IndexOutOfBoundsException", std::to_string(k));
    return items->getAt(ctx, static_cast<int>(k));
}

// The kwArgs map of a CALL_KW: the `kvItems` key/value arguments in the
// frame slots starting at `firstPairSlot`, folded in source order (a repeated
// key keeps its last value). The pairs stay rooted in their slots while the
// map is built, at most kChunkItems at a time. After each chunk the map
// built so far is rooted in the first pair's slot, which no later chunk
// reads, before the next chunk allocates. Out of line so the chunk buffer
// does not enlarge the native frame of every call (StackGuard.h).
[[gnu::noinline]] PROTOCLJ_MSVC_NOINLINE
const proto::ProtoObject* foldKeywordPairs(proto::ProtoContext* frame,
                                           unsigned int firstPairSlot,
                                           unsigned int kvItems) {
    constexpr unsigned int kChunkItems = 256;  // even
    const proto::ProtoObject* kv[kChunkItems];
    const proto::ProtoObject* kwMap = nullptr;
    for (unsigned int done = 0; done < kvItems; done += kChunkItems) {
        const unsigned int items = std::min(kChunkItems, kvItems - done);
        for (unsigned int i = 0; i < items; ++i) {
            kv[i] = frame->getAutomaticLocal(firstPairSlot + done + i);
        }
        kwMap = mapAssocPairs(frame, kwMap, kv, items);
        frame->setAutomaticLocal(firstPairSlot, kwMap);
    }
    return kwMap;
}

// An active exception handler of a frame (TRY_BEGIN / TRY_END).
struct TryHandler {
    const Instr* target;   // first instruction of the handler code
    unsigned int sp;       // operand-stack depth at TRY_BEGIN
};

// TRY_BEGIN's push, out of line: the vector's growth path inlined into the
// dispatch loop changed the register allocation of the whole loop and cost
// +5.5 % instructions on benchmarks/sum-loop.clj, which runs no `try` at all.
[[gnu::noinline]] PROTOCLJ_MSVC_NOINLINE
void pushTryHandler(std::vector<TryHandler>& handlers, const Instr* target,
                    unsigned int sp) {
    handlers.push_back(TryHandler{target, sp});
}

// THROW: raises `v` (rooted by the caller) as an exception; a value that is
// neither a Clojure exception nor a foreign object raises a
// ClassCastException, as JVM Clojure's `throw` of a non-Throwable does. Cold
// and out of line, to keep the dispatch loop small.
[[noreturn, gnu::cold, gnu::noinline]]
void throwValue(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
    if (!isThrowable(ctx, v)) {
        throwClassed(ctx, "ClassCastException",
            std::string("throw expects an exception, got ") + valueTypeName(ctx, v));
    }
    throwException(ctx, v);
}

// RETHROW: raises `v` (rooted by the caller) unchanged, whatever it is: a
// value a module raised need not be one `throw` would accept.
[[noreturn, gnu::cold, gnu::noinline]]
void rethrowValue(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
    throw proto::ProtoException(ctx, v);
}

// STORE_CAUGHT's frame-state half, out of line: the vector's growth path
// stays out of the dispatch loop (see pushTryHandler).
[[gnu::noinline]] PROTOCLJ_MSVC_NOINLINE
void storeCaught(std::vector<std::exception_ptr>& caught, std::exception_ptr& justCaught,
                 unsigned int slot) {
    if (caught.size() <= slot) caught.resize(slot + 1);
    caught[slot] = std::move(justCaught);
    justCaught = nullptr;
}

// The prefix and the member of a qualified name `a/b`.
void splitQualified(const std::string& name, std::string& prefix, std::string& member) {
    const std::size_t slash = name.find('/');
    prefix = name.substr(0, slash);
    member = name.substr(slash + 1);
}

// Whether `obj` has an attribute `key`, its prototype chain included. A
// read answers PROTO_NONE both for a missing attribute and for one bound to
// nil, so a PROTO_NONE read is confirmed by a presence probe.
bool readMember(proto::ProtoContext* ctx, const proto::ProtoObject* obj,
                const proto::ProtoString* key, const proto::ProtoObject*& out) {
    if (!obj || obj == PROTO_NONE ||
        (reinterpret_cast<std::uintptr_t>(obj) & 0x3F) != 0)
        return false;
    out = obj->getAttribute(ctx, key);
    if (out && out != PROTO_NONE) return true;
    if (obj->hasAttribute(ctx, key) == PROTO_TRUE) { out = PROTO_NONE; return true; }
    return false;
}

// GET_MEMBER: attribute `key` of `obj`, as `a/b` reads it.
[[gnu::noinline]]
const proto::ProtoObject* getMember(proto::ProtoContext* ctx, const proto::ProtoObject* obj,
                                    const proto::ProtoString* key, const std::string& name) {
    const proto::ProtoObject* v = nullptr;
    if (!readMember(ctx, obj, key, v))
        throwClassed(ctx, "RuntimeException",
            "No such member: " + name + " (" + valueTypeName(ctx, obj) + ")");
    return v;
}

// RESOLVE_CLASS: the type a catch clause names, resolved when an exception
// reaches the clause: the global `X`, or member `b` of the global `a`.
[[gnu::noinline, gnu::cold]]
const proto::ProtoObject* resolveCatchType(proto::ProtoContext* ctx,
                                           const proto::ProtoObject* globals,
                                           const std::string& name) {
    const proto::ProtoObject* v = nullptr;
    bool found = false;
    if (name.find('/') != std::string::npos && name.size() > 2) {
        std::string prefix, member;
        splitQualified(name, prefix, member);
        const proto::ProtoObject* owner = globals->getOwnAttributeDirect(
            ctx, proto::ProtoString::createSymbol(ctx, prefix.c_str()));
        found = owner && readMember(ctx, owner,
            proto::ProtoString::createSymbol(ctx, member.c_str()), v);
    } else {
        v = globals->getOwnAttributeDirect(ctx, proto::ProtoString::createSymbol(ctx, name.c_str()));
        found = v != nullptr;
    }
    if (!found) throwClassed(ctx, "RuntimeException", "Unable to resolve classname: " + name);
    return v;
}

// EXC_MATCH_TYPE: whether the catch of `type` (resolved at run time) handles
// the thrown `v`. A type that is not a prototype object (a number, a string,
// a Clojure collection, nil) is an error, not a silent non-match.
[[gnu::noinline, gnu::cold]]
bool matchCatchType(proto::ProtoContext* ctx, const proto::ProtoObject* v,
                    const proto::ProtoObject* type, const std::string& name) {
    if (!isForeignObject(ctx, type) && !isException(ctx, type))
        throwClassed(ctx, "IllegalArgumentException",
            "catch: " + name + " is " + valueTypeName(ctx, type) +
            ", not an exception type");
    return valueIsInstanceOf(ctx, v, type);
}

// CALL_MEMBER: `(a/b args...)`. A protoCore method (a module's export) is
// called with the receiver as `self`, the way a module expects; any other
// member value is called as a Clojure fn. `args` are the frame's stack
// slots, which the collector traces; the method runs in the frame `ctx`
// itself, with the frame's free slots above the arguments as its
// PrimitiveWindow (CallerSlots.h), and no context of its own. A C++
// exception that is not a ProtoException (a module defect) becomes a
// RuntimeException carrying its what() here, at the native-call boundary.
[[gnu::noinline]]
const proto::ProtoObject* callMember(proto::ProtoContext* ctx, ExecutionEngine* engine,
                                     const proto::ProtoObject* receiver,
                                     const proto::ProtoString* key, const std::string& name,
                                     const proto::ProtoObject* const* args, unsigned int argc) {
    const proto::ProtoObject* member = getMember(ctx, receiver, key, name);
    if (!member->isMethod(ctx))
        return engine->invoke(ctx, member, args, argc);
    const proto::ProtoArguments positional(args, argc);
    const proto::ProtoObject** frameSlots = ctx->getAutomaticLocals();
    PrimitiveWindow window{ctx, const_cast<const proto::ProtoObject**>(args) + argc,
                           frameSlots + ctx->getAutomaticLocalsCount(), 1};
    PrimitiveCallScope callScope(window);
    // A ProtoException is no std::exception: it passes through untouched.
    try {
        return member->asMethod(ctx)(ctx, receiver, nullptr, &positional, nullptr);
    } catch (const std::exception& e) {
        throwClassed(ctx, "RuntimeException", e.what());
    }
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
                          const proto::ProtoObject** out) {
    const auto& kkeys = subMod->kwKeys();
    if (!isMap(maybeMap)) {
        for (std::size_t i = 0; i < kkeys.size(); ++i) out[i] = PROTO_NONE;
        return false;
    }
    for (std::size_t i = 0; i < kkeys.size(); ++i) {
        // Look the keyword `:name` (interned by the compiler) up, as
        // prim_get does.
        bool found = false;
        const proto::ProtoObject* v =
            mapGet(ctx, maybeMap, kkeys[i].keyword, &found);
        out[i] = found ? v : PROTO_NONE;
    }
    return true;
}

// Helper: pick which arity-spec inside a multi-arity wrapper to use for
// a given argc. First pass: exact fixed match. Second pass: variadic with
// argc >= minArity. Returns the index into the __arities__ list, or -1.
static int pickArity(proto::ProtoContext* ctx,
                     const proto::ProtoList* aritiesList,
                     unsigned int argc) {
    proto::proto_ulong N = aritiesList->getSize(ctx);
    for (proto::proto_ulong k = 0; k < N; ++k) {
        const proto::ProtoList* spec =
            aritiesList->getAt(ctx, (int)k)->asList(ctx);
        long long a = spec->getAt(ctx, 0)->asLong(ctx);
        bool variadic = spec->getAt(ctx, 2) == PROTO_TRUE;
        if (!variadic && a == static_cast<long long>(argc)) return (int)k;
    }
    for (proto::proto_ulong k = 0; k < N; ++k) {
        const proto::ProtoList* spec =
            aritiesList->getAt(ctx, (int)k)->asList(ctx);
        long long a = spec->getAt(ctx, 0)->asLong(ctx);
        bool variadic = spec->getAt(ctx, 2) == PROTO_TRUE;
        if (variadic && static_cast<long long>(argc) >= a) return (int)k;
    }
    // Third pass: a keyword-argument arity (`[a & {:keys ...}]`) takes its
    // fixed parameters and then key/value pairs, or one trailing map.
    for (proto::proto_ulong k = 0; k < N; ++k) {
        const proto::ProtoList* spec =
            aritiesList->getAt(ctx, (int)k)->asList(ctx);
        const auto* sub = reinterpret_cast<const BytecodeModule*>(spec->getAt(ctx, 1)->asLong(ctx));
        if (!sub->isKwBased()) continue;
        const long long a = sub->arity();
        const long long extra = static_cast<long long>(argc) - a;
        if (extra >= 0 && (extra % 2 == 0 || extra == 1)) return (int)k;
    }
    return -1;
}

// See the contract on the declarations in ExecutionEngine.h.
const bool ExecutionEngine::gcSafepointEnabled_ =
    (std::getenv("PROTOCLJ_NO_GC_SAFEPOINT") == nullptr);

thread_local unsigned ExecutionEngine::gcSafepointCountdown_ = 1;

// See the declaration: one test when the frame allocated nothing since its
// last submission, inlined into the call path.
[[gnu::always_inline]] PROTOCLJ_INLINE void
ExecutionEngine::submitBeforeCall(proto::ProtoContext& frame) const {
    if (frame.lastAllocatedCell) [[unlikely]] submitBeforeCallSlow(frame);
}

[[gnu::noinline]] PROTOCLJ_MSVC_NOINLINE
void ExecutionEngine::submitBeforeCallSlow(proto::ProtoContext& frame) const {
    // See the declaration. The chain is detached and submitted the way
    // ~ProtoContext does it (ProtoSpace::submitYoungGeneration); the frame
    // stays alive and keeps allocating into a fresh chain. Skipped inside a
    // critical section, where a half-built tree may hang off the chain.
    if (!gcSafepointEnabled_ || frame.criticalSectionDepth != 0) return;
    proto::Cell* chain = frame.lastAllocatedCell;
    frame.lastAllocatedCell = nullptr;
    frame.allocatedCellsCount = 0;
    frame.space->submitYoungGeneration(chain);
    frame.safepoint();
}

void ExecutionEngine::gcSafepointSlow(proto::ProtoContext* ctx) const {
    // Reload first, unconditionally: leaving the countdown at zero would wrap
    // it to UINT_MAX on the next event and silence the hook for the rest of
    // the thread's life. The reload happens whether or not the poll does, so
    // PROTOCLJ_NO_GC_SAFEPOINT=1 leaves the loop's instruction mix as close to
    // the enabled build's as the escape hatch can.
    gcSafepointCountdown_ = kGcSafepointStride;
    if (gcSafepointEnabled_ && ctx) ctx->safepoint();
}

// Declared in ExecutionEngine.h. Defined before its callers, so it inlines
// into them: the choice costs one flag test and no native frame.
[[gnu::always_inline]] PROTOCLJ_INLINE const proto::ProtoObject*
ExecutionEngine::execute(proto::ProtoContext* parent,
                         const BytecodeModule& mod,
                         const ActiveCallContext& env,
                         const proto::ProtoObject* const* args,
                         unsigned int argCount,
                         const proto::ProtoObject* captures,
                         const proto::ProtoObject* const* kwVals,
                         unsigned int kwCount,
                         const proto::ProtoObject* kwMap,
                         const proto::ProtoObject** resultSlot) {
    if (mod.hasHandlers()) [[unlikely]] {
        return executeFrame<true>(parent, mod, env, args, argCount, captures,
                                  kwVals, kwCount, kwMap, resultSlot);
    }
    return executeFrame<false>(parent, mod, env, args, argCount, captures,
                               kwVals, kwCount, kwMap, resultSlot);
}

const proto::ProtoObject*
ExecutionEngine::invoke(proto::ProtoContext* ctx,
                        const proto::ProtoObject* callable,
                        const proto::ProtoObject* const* args,
                        unsigned int argc) {
    // A primitive that reads its arguments without running Clojure code
    // and without allocating (fastPrimitiveCall): no context of its own.
    // Nothing is allocated in `ctx`, which may be a long-lived scope.
    if ((reinterpret_cast<proto::proto_ulong>(callable) & 0x3F) == kTagMethod) {
        if (const proto::ProtoObject* r = fastPrimitiveCall(
                ctx, callable->asMethod(ctx), args, argc, false))
            return r;
    }

    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throwError(ctx, "VM: invoke() called outside a run()");

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
                callable->getOwnAttributeDirect(ctx, cc->aritiesKey);
            const proto::ProtoList* aritiesList = aritiesObj->asList(ctx);
            int k = pickArity(ctx, aritiesList, argc);
            if (k < 0)
                throwError(ctx, 
                    "VM: invoke: no matching arity for argc=" + std::to_string(argc));
            const proto::ProtoList* spec =
                aritiesList->getAt(ctx, k)->asList(ctx);
            subMod = reinterpret_cast<const BytecodeModule*>(
                spec->getAt(ctx, 1)->asLong(ctx));
            capsVal = spec->getAt(ctx, 3);
        } else {
            const proto::ProtoObject* ptrObj =
                callable->getOwnAttributeDirect(ctx, cc->bytecodeKey);
            if (!ptrObj || !ptrObj->isInteger(ctx))
                throwError(ctx, "VM: invoke: malformed fn-wrapper");
            subMod = reinterpret_cast<const BytecodeModule*>(ptrObj->asLong(ctx));
            // C optimisation — only fetch captures when the body needs them.
            if (subMod->captureCount() > 0) {
                capsVal = callable->getOwnAttributeDirect(ctx, cc->capturesKey);
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
            if (static_cast<int>(posArgc) > fixed + 1 && (posArgc - fixed) % 2 == 0) {
                // Key/value pairs after the fixed arguments (apply, or a
                // primitive calling the fn): folded into the kwArgs map.
                kwArgsMap = mapAssocPairs(ctx, nullptr, args + fixed, posArgc - fixed);
                posArgc = static_cast<unsigned int>(fixed);
            } else if (static_cast<int>(posArgc) == fixed + 1) {
                kwArgsMap = args[posArgc - 1];
                posArgc -= 1;
            } else if (static_cast<int>(posArgc) != fixed) {
                throwError(ctx, 
                    "VM: invoke: kw-based fn expects " + std::to_string(fixed) +
                    " positionals (+ optional trailing map), got " +
                    std::to_string(argc));
            }
        } else {
            if (!variadic && fixed != static_cast<int>(posArgc))
                throwError(ctx, "VM: invoke: fn arity mismatch");
            if (variadic && static_cast<int>(posArgc) < fixed)
                throwError(ctx, "VM: invoke: variadic underflow");
        }
        const proto::ProtoObject* callArgs[kMaxBoundParameters];
        unsigned int passArgc = posArgc;
        if (variadic) passArgc = static_cast<unsigned int>(fixed) + 1;
        if (passArgc > kMaxBoundParameters)
            throwError(ctx, 
                "VM: a call may bind at most 17 parameters "
                "(a variadic fn binds its rest arguments as one)");
        for (int i = 0; i < fixed; ++i) callArgs[i] = args[i];
        if (variadic) {
            // The rest arguments as a list (a seq, as Clojure's rest
            // parameter); none binds the rest parameter to nil, as in Clojure.
            callArgs[fixed] = static_cast<unsigned int>(fixed) < posArgc
                ? listFromArray(ctx, posArgc - static_cast<unsigned int>(fixed), args + fixed)
                : PROTO_NONE;
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
                throwError(ctx, "VM: a function may declare at most 16 :keys parameters");
            extractKwVals(ctx, subMod, kwArgsMap, kwVals);
        }

        return execute(ctx, *subMod, *cc, callArgs, passArgc, capsVal,
                       kwBased ? kwVals : nullptr, kwCount,
                       kwBased ? kwArgsMap : nullptr);
    }

    // Keywords, quoted symbols and maps are functions: a map lookup.
    if (proto == cc->named.marker || isMap(callable) || (proto && proto == setPrototype())) {
        return callLookup(ctx, cc->named, callable,
                          proto != cc->named.marker, args, argc);
    }

    // C++ primitive. It runs in the caller's context `ctx`, with no context
    // of its own (CallerSlots.h): `callable` and `args` are rooted by the
    // caller (invoke's contract), and nothing submits `ctx`'s young chain
    // while the primitive runs, because it counts as a nested activation on
    // the current PrimitiveWindow and so never safepoints.
    proto::ProtoMethod fn = callable->asMethod(ctx);
    if (!fn && isVector(callable)) return callVector(ctx, callable, args, argc);
    if (!fn) throwError(ctx, "VM: invoke: value is not callable");
    const proto::ProtoObject* self = callable->asMethodSelf(ctx);
    const proto::ProtoArguments positional(args, argc);
    NestedPrimitiveScope nested;
    return fn(ctx, self, nullptr, &positional, nullptr);
}

bool ExecutionEngine::acceptsArgCount(proto::ProtoContext* ctx,
                                      const proto::ProtoObject* callable,
                                      unsigned int argc) const {
    const ActiveCallContext* cc = activeCallContext();
    if (!cc || !callable || callable == PROTO_NONE) return true;
    const proto::ProtoObject* proto = callable->getPrototype(ctx);
    if (proto == cc->fnMultiProto) {
        const proto::ProtoObject* aritiesObj =
            callable->getOwnAttributeDirect(ctx, cc->aritiesKey);
        return pickArity(ctx, aritiesObj->asList(ctx), argc) >= 0;
    }
    if (proto != cc->fnSingleProto) return true;
    const proto::ProtoObject* ptrObj =
        callable->getOwnAttributeDirect(ctx, cc->bytecodeKey);
    if (!ptrObj || !ptrObj->isInteger(ctx)) return true;
    const auto* subMod = reinterpret_cast<const BytecodeModule*>(ptrObj->asLong(ctx));
    const int fixed = subMod->arity();
    if (subMod->isKwBased())
        return static_cast<int>(argc) == fixed || static_cast<int>(argc) == fixed + 1;
    if (subMod->isVariadic()) return static_cast<int>(argc) >= fixed;
    return static_cast<int>(argc) == fixed;
}

const proto::ProtoObject*
ExecutionEngine::run(proto::ProtoContext* parent,
                     const BytecodeModule& mod,
                     const proto::ProtoObject* globals,
                     const proto::ProtoObject* fnSingleProto,
                     const proto::ProtoObject* fnMultiProto,
                     const proto::ProtoObject* atomMarkerProto,
                     const proto::ProtoObject* futureMarkerProto,
                     const proto::ProtoObject* promiseMarkerProto,
                     const proto::ProtoObject* actorMarkerProto,
                     const proto::ProtoString* bytecodeKey,
                     const proto::ProtoString* arityKey,
                     const proto::ProtoString* capturesKey,
                     const proto::ProtoString* aritiesKey,
                     const proto::ProtoString* valueKey,
                     const proto::ProtoString* watchesKey,
                     const proto::ProtoString* thunkKey,
                     const proto::ProtoString* ccBlobKey,
                     const proto::ProtoString* threadKey,
                     const proto::ProtoString* resultKey,
                     const proto::ProtoString* doneKey,
                     const proto::ProtoString* actorStateKey,
                     const proto::ProtoString* mailboxKey,
                     const NamedLayout& named,
                     const proto::ProtoObject* const* args,
                     unsigned int argCount,
                     const proto::ProtoObject* captures,
                     const proto::ProtoObject* const* kwVals,
                     unsigned int kwCount,
                     const proto::ProtoObject* kwMap) {
    // Top-level entry (the script driver, the REPL). Install this run's
    // call context on the thread and restore the prior one (the REPL's
    // printer context, or none) on every exit path. Nested calls, from the
    // VM or from a primitive through invoke(), go to execute() directly and
    // share the installed context.
    const ActiveCallContext* prior = activeCallContext();
    ActiveCallContext saved = prior ? *prior : ActiveCallContext{};
    const ActiveCallContext cc{this, globals,
                         fnSingleProto, fnMultiProto,
                         atomMarkerProto, futureMarkerProto, promiseMarkerProto,
                         actorMarkerProto,
                         bytecodeKey, arityKey, capturesKey, aritiesKey,
                         valueKey, watchesKey,
                         thunkKey, ccBlobKey, threadKey, resultKey, doneKey,
                         actorStateKey, mailboxKey, named};
    setActiveCallContext(cc);
    struct Guard {
        const ActiveCallContext* prior; ActiveCallContext saved;
        ~Guard() {
            if (prior) setActiveCallContext(saved);
            else clearActiveCallContext();
        }
    } guard{prior, saved};

    return execute(parent, mod, cc, args, argCount, captures,
                   kwVals, kwCount, kwMap);
}

template <bool kHandlers>
const proto::ProtoObject*
ExecutionEngine::executeFrame(proto::ProtoContext* parent,
                              const BytecodeModule& mod,
                              const ActiveCallContext& env,
                              const proto::ProtoObject* const* args,
                              unsigned int argCount,
                              const proto::ProtoObject* captures,
                              const proto::ProtoObject* const* kwVals,
                              unsigned int kwCount,
                              const proto::ProtoObject* kwMap,
                              const proto::ProtoObject** resultSlot) {
    // Every call nests here on the native stack: raise StackOverflowError
    // instead of recursing past the end of the thread's stack.
    checkNativeStack();

    // One ProtoContext per frame. Layout of automaticLocals:
    //   [0 .. arity-1]                         — params (bound at call)
    //   [arity .. arity+localCount-1]          — lets / loops
    //   [arity+localCount .. + stackCapacity-1]  — operand stack (grows)
    const unsigned int locBase   = 0;
    const unsigned int stackBase = static_cast<unsigned int>(mod.arity()
                                                              + mod.localCount());
    // The operand stack's capacity is the deepest the compiled body gets
    // (BytecodeModule::maxStack); it still grows below if CALL_APPLY spreads
    // a long list.
    unsigned int stackCapacity = mod.maxStack();

    // Built at its full initial size: the slots come from the thread's slot
    // stack once, instead of an empty context grown by resizeAutomaticLocals.
    // kPrimitiveSlots more above the operand stack are the floor of the
    // PrimitiveWindow of the primitives this frame calls (CallerSlots.h).
    proto::ProtoContext frame(parent->space, parent, nullptr, nullptr, nullptr, nullptr,
                              stackBase + stackCapacity + kPrimitiveSlots);
    unsigned int sp = 0;   // 0 means "stack empty"; sp <= stackCapacity

    // A body compiled for the debugger is a frame of the debugger's call
    // stack while it runs (DebugHook.h); any other costs this one test.
    DebugFrameScope debugScope(mod, &frame);

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
            throwError(&frame, 
                "VM: fn expects captures but none were provided");
        }
        const proto::ProtoList* clist = captures->asList(&frame);
        if (!clist || clist->getSize(&frame) != capSpecs.size()) {
            throwError(&frame, 
                "VM: captures list size mismatch with body captureSpecs");
        }
        for (std::size_t i = 0; i < capSpecs.size(); ++i) {
            frame.setAutomaticLocal(locBase + capSpecs[i].localSlot,
                clist->getAt(&frame, static_cast<int>(i)));
        }
    }

    // Garbage-collection safepoint on frame entry. Every incoming value —
    // arguments, `:keys` values, the `:as` map, the captures — is already
    // bound into a traced frame slot, so this poll cannot expose an unrooted
    // object. It submits nothing (this frame has allocated nothing yet); it
    // is here because `safepoint()` is also protoCore's stop-the-world park
    // point, and recursion over the SmallInt fast-path opcodes allocates
    // nothing at all. See ExecutionEngine.h::gcSafepoint.
    gcSafepoint(&frame);

    // No capacity test per push: the capacity is the body's maxStack, the
    // deepest any instruction takes the stack, and CALL_APPLY reserves room
    // before it spreads a list (reserveStack). setAutomaticLocal still
    // refuses an index past the slots.
    auto pushVal = [&](const proto::ProtoObject* v) {
        frame.setAutomaticLocal(stackBase + sp, v);
        ++sp;
    };
    // Room for `extra` more values: resizeAutomaticLocals copies the slots
    // into a larger buffer and allocates no protoCore cell, so no collection
    // can run in between.
    auto reserveStack = [&](proto::proto_ulong extra) {
        if (sp + extra <= stackCapacity) return;
        stackCapacity = static_cast<unsigned int>(sp + extra);
        frame.resizeAutomaticLocals(stackBase + stackCapacity + kPrimitiveSlots);
    };
    auto popVal = [&]() -> const proto::ProtoObject* {
        if (sp == 0) {
            throwError(&frame, "VM: operand-stack underflow");
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
        // A primitive (a protoCore method value) whose arguments it reads
        // without running Clojure code: answered here, with no context of
        // its own (fastPrimitiveCall). Its arguments stay rooted in their
        // slots until the result replaces them, and what it allocates is
        // this frame's garbage, handed to the collector at the next loop
        // back-edge, as the arithmetic opcodes' is.
        if ((reinterpret_cast<proto::proto_ulong>(callable) & 0x3F) == kTagMethod) {
            const proto::ProtoObject** argSlots =
                frame.getAutomaticLocals() + stackBase + sp - argc;
            if (const proto::ProtoObject* r = fastPrimitiveCall(
                    &frame, callable->asMethod(&frame), argSlots, argc, true)) {
                for (unsigned int i = 0; i < argc; ++i) argSlots[i] = nullptr;
                sp -= (argc + 1);
                pushVal(r);
                return;
            }
        }
        const proto::ProtoObject* proto = callable->getPrototype(&frame);
        const bool isSingle = (proto == env.fnSingleProto);
        const bool isMulti  = (proto == env.fnMultiProto);

        if (isSingle || isMulti) {
            // The frame is suspended for the call, with every value it holds
            // in its slots (the arguments too, until they are read below):
            // what it and its primitives allocated goes to the collector
            // now, so a value built here and walked by the callee is not
            // kept alive by this frame's young chain (submitBeforeCall).
            submitBeforeCall(frame);
            const BytecodeModule* subMod = nullptr;
            const proto::ProtoObject* capsVal = nullptr;

            if (isMulti) {
                // Multi-arity — read __arities__ and pick the spec.
                const proto::ProtoObject* aritiesObj =
                    callable->getOwnAttributeDirect(&frame, env.aritiesKey);
                const proto::ProtoList* aritiesList = aritiesObj->asList(&frame);
                int k = pickArity(&frame, aritiesList, argc);
                if (k < 0)
                    throwError(&frame, 
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
                    callable->getOwnAttributeDirect(&frame, env.bytecodeKey);
                if (!ptrObj || !ptrObj->isInteger(&frame))
                    throwError(&frame, "VM: malformed fn-wrapper");
                subMod = reinterpret_cast<const BytecodeModule*>(
                    ptrObj->asLong(&frame));
                if (subMod->captureCount() > 0) {
                    capsVal = callable->getOwnAttributeDirect(&frame, env.capturesKey);
                }
            }

            int fixed = subMod->arity();
            bool variadic = subMod->isVariadic();
            bool kwBased  = subMod->isKwBased();

            // A keyword-argument callee given key/value pairs after its
            // fixed arguments by CALL or CALL_APPLY, or by CALL_KW to a
            // multi-arity fn: the pairs are folded into the kwArgs map in
            // the first pair's slot, as CALL_KW does for a single-arity fn.
            if (kwBased && argc > static_cast<unsigned int>(fixed) + 1 &&
                (argc - static_cast<unsigned int>(fixed)) % 2 == 0) {
                const unsigned int kv = argc - static_cast<unsigned int>(fixed);
                foldKeywordPairs(&frame, stackBase + sp - kv, kv);
                for (unsigned int i = 1; i < kv; ++i)
                    frame.setAutomaticLocal(stackBase + sp - kv + i, nullptr);
                sp -= kv - 1;
                argc = static_cast<unsigned int>(fixed) + 1;
            }

            const proto::ProtoObject* kwArgsMap = nullptr;
            unsigned int posArgc = argc;
            if (kwBased) {
                if (static_cast<int>(posArgc) == fixed + 1) {
                    kwArgsMap = frame.getAutomaticLocal(
                        stackBase + sp - 1);
                    posArgc -= 1;
                } else if (static_cast<int>(posArgc) != fixed) {
                    throwError(&frame, 
                        "VM: kw-based fn expects " + std::to_string(fixed) +
                        " positionals (+ optional trailing map), got " +
                        std::to_string(argc));
                }
            } else {
                if (!variadic && fixed != static_cast<int>(posArgc)) {
                    throwError(&frame, 
                        "VM: fn arity mismatch (expected " +
                        std::to_string(fixed) +
                        ", got " + std::to_string(argc) + ")");
                }
                if (variadic && static_cast<int>(posArgc) < fixed) {
                    throwError(&frame, 
                        "VM: variadic fn needs at least " +
                        std::to_string(fixed) + " args, got " +
                        std::to_string(argc));
                }
            }
            const proto::ProtoObject* callArgs[kMaxBoundParameters];
            unsigned int passArgc = posArgc;
            if (variadic) passArgc = static_cast<unsigned int>(fixed) + 1;
            if (passArgc > kMaxBoundParameters)
                throwError(&frame, 
                    "VM: a call may bind at most 17 parameters "
                    "(a variadic fn binds its rest arguments as one)");
            // Each argument moves from its stack slot to callArgs, which the
            // callee binds into its own frame before anything allocates (the
            // frame constructor takes slots, not cells); the slot is cleared
            // as it is read, so a lazy seq passed down is held by the callee
            // alone, which locals clearing may then release (docs/DESIGN.md
            // §10.2, M30). The rest arguments are cleared once they are in
            // the rest list, after the kw map has been read.
            const proto::ProtoObject** argSlots =
                frame.getAutomaticLocals() + stackBase + sp - argc;
            // The rest arguments as a list (a seq, as Clojure's rest
            // parameter), built while every argument is still in its slot:
            // building it allocates. No rest arguments binds the rest
            // parameter to nil, as in Clojure.
            const proto::ProtoObject* restList = PROTO_NONE;
            if (variadic && static_cast<unsigned int>(fixed) < posArgc)
                restList = listFromArray(&frame, posArgc - static_cast<unsigned int>(fixed),
                                         argSlots + fixed);
            for (int i = 0; i < fixed; ++i) {
                callArgs[i] = argSlots[i];
                argSlots[i] = nullptr;
            }
            if (variadic) {
                callArgs[fixed] = restList;
                for (unsigned int i = static_cast<unsigned int>(fixed); i < posArgc; ++i)
                    argSlots[i] = nullptr;
            } else if (!kwBased) {
                for (unsigned int i = static_cast<unsigned int>(fixed);
                     i < posArgc; ++i) {
                    callArgs[i] = argSlots[i];
                    argSlots[i] = nullptr;
                }
            }

            const proto::ProtoObject* kwVals[16];
            unsigned int kwCount = 0;
            if (kwBased) {
                kwCount = static_cast<unsigned int>(subMod->kwKeys().size());
                if (kwCount > 16)
                    throwError(&frame, "VM: a function may declare at most 16 :keys parameters");
                extractKwVals(&frame, subMod, kwArgsMap, kwVals);
            }

            // The arguments now live in callArgs / kwVals, which the callee
            // binds into its own frame before anything allocates (the frame
            // constructor takes slots, not cells). Clearing the caller's
            // copies means a lazy seq passed down is held by the callee
            // alone, which may then let its head go (locals clearing, M30).
            sp -= (argc + 1);
            // The callable's slot (now the top of the stack) receives the
            // result before the callee's frame ends; pushVal then writes the
            // same value there. The caller's slots do not move during the
            // call: only this frame's own pushes resize them.
            const proto::ProtoObject* result = execute(
                &frame, *subMod, env, callArgs, passArgc, capsVal,
                kwBased ? kwVals : nullptr, kwCount,
                kwBased ? kwArgsMap : nullptr,
                frame.getAutomaticLocals() + stackBase + sp);
            pushVal(result);
            return;
        }

        // A keyword, a quoted symbol or a map in call position looks a key
        // up, as `get` does (callLookup). The arguments stay rooted in their
        // stack slots until the lookup returns.
        const bool callableIsNamed = proto == env.named.marker;
        const bool callableIsMap =
            !callableIsNamed && (isMap(callable) || (proto && proto == setPrototype()));
        if (callableIsNamed || callableIsMap) {
            const proto::ProtoObject* lookupArgs[2] = {nullptr, nullptr};
            for (unsigned int i = 0; i < argc && i < 2; ++i) {
                lookupArgs[i] = frame.getAutomaticLocal(stackBase + sp - argc + i);
            }
            const proto::ProtoObject* result = callLookup(
                &frame, env.named, callable, callableIsMap,
                lookupArgs, argc);
            sp -= (argc + 1);
            pushVal(result);
            return;
        }

        // C++ primitive. The callable and its arguments already sit
        // contiguously in the frame's operand stack, which the collector
        // traces: the primitive reads them there (protoCore's array call
        // form), nothing is packed or copied. It runs in this frame, with no
        // context of its own (CallerSlots.h): what it allocates is this
        // frame's garbage, handed to the collector at the frame's next
        // safepoint, and the free slots above its arguments are its
        // PrimitiveWindow.
        proto::ProtoMethod fn = callable->asMethod(&frame);
        if (!fn && isVector(callable)) {
            const proto::ProtoObject* vargs[1] = {
                argc >= 1 ? frame.getAutomaticLocal(stackBase + sp - argc) : nullptr};
            const proto::ProtoObject* result = callVector(&frame, callable, vargs, argc);
            sp -= (argc + 1);
            pushVal(result);
            return;
        }
        if (!fn)
            throwError(&frame, "VM: value is not callable in CALL");
        const proto::ProtoObject* self = callable->asMethodSelf(&frame);
        const proto::ProtoObject** frameSlots = frame.getAutomaticLocals();
        const proto::ProtoArguments positional(frameSlots + stackBase + sp - argc, argc);
        const proto::ProtoObject* result;
        {
            PrimitiveWindow window{&frame, frameSlots + stackBase + sp,
                                   frameSlots + frame.getAutomaticLocalsCount(), 1};
            PrimitiveCallScope callScope(window);
            result = fn(&frame, self, nullptr, &positional, nullptr);
        }
        // The argument slots are dead now; clear them so they do not keep a
        // value (the head of a lazy seq the primitive walked) alive until
        // the stack grows back over them.
        {
            const proto::ProtoObject** argSlots = frame.getAutomaticLocals() + stackBase + sp - argc;
            for (unsigned int i = 0; i < argc; ++i) argSlots[i] = nullptr;
        }
        sp -= (argc + 1);
        pushVal(result);
    };

    // The module is complete before it runs, so its code buffer is stable.
    // `ip` points at the next instruction word; jumps move it by their
    // operand, counted in instructions.
    const Instr* const codeEnd = mod.code().data() + mod.code().size();
    const Instr* ip = mod.code().data();

    // The frame's active exception handlers, innermost last (TRY_BEGIN /
    // TRY_END). Plain C++ data: a code position and a stack depth. Empty,
    // and never allocated, in a frame that runs no `try`.
    std::vector<TryHandler> handlers;
    if constexpr (kHandlers) {
        // The debugger asks a throw's frames whether they catch it.
        if (debugScope.active()) [[unlikely]] debugScope.frame().handlers = &handlers;
    }

    // The C++ exception each catch slot holds (STORE_CAUGHT / RETHROW),
    // indexed by local slot, and the one just caught, waiting for the
    // handler's STORE_CAUGHT. Frame state: a rethrow raises the very
    // exception object this frame caught, never a copy, whatever runtime
    // subclass of ProtoException it is. Empty in a frame that runs no `try`.
    std::vector<std::exception_ptr> caught;
    std::exception_ptr justCaught;

    // The dispatch loop. Returns the frame's value at RETURN or at the end
    // of the code.
    auto dispatchLoop = [&]() PROTOCLJ_LAMBDA_ALWAYS_INLINE -> const proto::ProtoObject* {
    // The argument count of the CALL being dispatched; an operator opcode
    // whose global was redefined jumps to CALL's dispatch with 2.
    unsigned int callArgc = 0;
    while (ip < codeEnd) {
        const Instr word = *ip++;
        const Op op = static_cast<Op>(word & 0xFF);
        const std::uint32_t operand = word >> kOperandShift;

        switch (op) {
            case Op::NOP:
                break;

            case Op::PUSH_CONST: {
                if (operand >= mod.constCount())
                    throwError(&frame, "VM: PUSH_CONST out-of-range");
                pushVal(materialise(&frame, mod.constAt(operand)));
                break;
            }

            case Op::PUSH_VAR: {
                if (operand >= mod.constCount())
                    throwError(&frame, "VM: PUSH_VAR out-of-range");
                const auto& c = mod.constAt(operand);
                if (c.kind != BytecodeModule::ConstKind::Symbol)
                    throwError(&frame, "VM: PUSH_VAR const is not a symbol");
                const auto* key = reinterpret_cast<const proto::ProtoString*>(c.named);
                // Every global is an own attribute of the globals object
                // (STORE_GLOBAL, installPrimitives), so one own read answers
                // both the value and its presence: nullptr is a missing
                // global, PROTO_NONE a global bound to nil.
                const proto::ProtoObject* v =
                    env.globals->getOwnAttributeDirect(&frame, key);
                if (!v) [[unlikely]] {
                    throwError(&frame, 
                        "VM: unable to resolve symbol: " + c.sval);
                }
                pushVal(v);
                break;
            }

            case Op::PUSH_LOCAL: {
                pushVal(frame.getAutomaticLocal(locBase + operand));
                break;
            }

            case Op::PUSH_LOCAL_CLEAR: {
                // The last read of a local (locals clearing, M30): the value
                // moves to the stack and the slot no longer holds it.
                pushVal(frame.getAutomaticLocal(locBase + operand));
                frame.setAutomaticLocal(locBase + operand, nullptr);
                break;
            }

            case Op::STORE_LOCAL: {
                if (sp == 0)
                    throwError(&frame, "VM: STORE_LOCAL with empty stack");
                frame.setAutomaticLocal(locBase + operand, popVal());
                break;
            }

            case Op::MAKE_FN: {
                if (operand >= mod.blockCount())
                    throwError(&frame, "VM: MAKE_FN out-of-range");
                const BytecodeModule& subMod = mod.block(operand);

                // Session 6 — pop captureCount values from the stack and
                // collect them into a ProtoList (top-of-stack = LAST
                // capture, walking back gives source order). The compiler
                // emitted exactly captureCount PUSH_LOCALs from the
                // enclosing scope, one per capture, in capture-spec order.
                int nCaps = subMod.captureCount();
                if (sp < static_cast<unsigned int>(nCaps)) {
                    throwError(&frame, "VM: MAKE_FN underflow on captures");
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

                // Single-arity wrapper — a child of fnSingleProto carrying
                // __bytecode__ always and __captures__ only when the body
                // has captures. A wrapper is never mutated once built, so
                // it is an immutable object: each setAttribute returns a
                // new object with the same prototype, and no wrapper enters
                // protoCore's mutables tree. The intermediates are young
                // cells of this frame's context until pushVal roots the
                // result.
                const proto::ProtoObject* wrap =
                    env.fnSingleProto->newChild(&frame)->setAttribute(
                        &frame, env.bytecodeKey,
                        frame.fromLong(reinterpret_cast<long long>(&subMod)));
                if (nCaps > 0) {
                    wrap = wrap->setAttribute(&frame, env.capturesKey, capsList);
                }
                pushVal(wrap);
                break;
            }

            case Op::CALL: {
                callArgc = operand;
                if (sp < callArgc + 1u)
                    throwError(&frame, "VM: CALL with insufficient stack");
            callDispatch:
                dispatchCall(callArgc);
                break;
            }

            case Op::CALL_APPLY: {
                // Stack: [..., callable, args-list]. Pop list, expand its
                // elements onto the stack as positional args, then dispatch
                // with argc = list size. Reuses the same dispatch as CALL.
                if (sp < 2)
                    throwError(&frame, "VM: CALL_APPLY underflow");
                // nil spreads no arguments and a vector its elements, as in
                // Clojure (`(apply vector (seq ()))` is `[]`).
                const proto::ProtoObject* listObj = popVal();
                if (isListValue(&frame, listObj)) {
                    // A list spreads its cells. Nothing below allocates (the
                    // stack grows without a cell), so the list stays intact
                    // while its slot is reused for the first argument.
                    const long long ln = listCount(&frame, listObj);
                    reserveStack(static_cast<proto::proto_ulong>(ln));
                    for (const proto::ProtoObject* p = listObj; p != emptyListValue();
                         p = listRest(&frame, p))
                        pushVal(listFirst(&frame, p));
                    dispatchCall(static_cast<unsigned int>(ln));
                    break;
                }
                const proto::ProtoList* argsList = nullptr;
                if (!listObj || listObj == PROTO_NONE) {
                    argsList = frame.newList();
                } else if (isVector(listObj)) {
                    argsList = vectorItems(&frame, listObj);
                } else {
                    // A lazy seq, a map, a set or a string spreads its seq
                    // (seqItems), realised as by a primitive the frame calls:
                    // under a PrimitiveWindow above the popped list's slot,
                    // which still holds it, and with a SafepointPermit, since
                    // everything the frame holds is in its slots, so a long
                    // realisation hands its garbage over (CallerSlots.h).
                    const proto::ProtoObject** frameSlots = frame.getAutomaticLocals();
                    PrimitiveWindow window{&frame, frameSlots + stackBase + sp + 1,
                                           frameSlots + frame.getAutomaticLocalsCount(), 1};
                    PrimitiveCallScope callScope(window);
                    SafepointPermit permit(&frame);
                    argsList = seqItems(&frame, listObj);
                }
                if (!argsList)
                    throwError(&frame, "VM: CALL_APPLY: not a list");
                proto::proto_ulong ln = argsList->getSize(&frame);
                reserveStack(ln);
                for (proto::proto_ulong i = 0; i < ln; ++i) {
                    pushVal(argsList->getAt(&frame, static_cast<int>(i)));
                }
                dispatchCall(static_cast<unsigned int>(ln));
                break;
            }

            case Op::CALL_KW: {
                // Stack: [..., callable, arg1, ..., argN]; operand = N.
                // The compiler emits CALL_KW instead of CALL when the call
                // ends in `:keyword value` pairs. Every argument, the pairs
                // included, is on the stack as a positional in source order.
                //
                // A non-keyword-argument callee (every primitive, every
                // ordinary fn) receives the arguments unchanged, exactly as
                // CALL would pass them: no reordering, no de-duplication.
                //
                // A keyword-argument callee (`& {:keys ...}`) takes its
                // declared positionals from the front; the remaining
                // arguments must be key/value pairs, folded into the kwArgs
                // map in source order (a repeated key keeps its last value,
                // as in Clojure). The split is decided by the callee's
                // arity, not by where the compiler saw the first keyword,
                // so keyword values in positional parameters work.
                unsigned int argc = operand;
                if (sp < argc + 1u)
                    throwError(&frame, "VM: CALL_KW with insufficient stack");
                const proto::ProtoObject* callable = peekAt(argc);
                const BytecodeModule* kwMod = nullptr;
                if (callable->getPrototype(&frame) == env.fnSingleProto) {
                    const proto::ProtoObject* ptrObj =
                        callable->getOwnAttributeDirect(&frame, env.bytecodeKey);
                    if (ptrObj && ptrObj->isInteger(&frame)) {
                        const BytecodeModule* subMod =
                            reinterpret_cast<const BytecodeModule*>(
                                ptrObj->asLong(&frame));
                        if (subMod->isKwBased()) kwMod = subMod;
                    }
                }
                if (!kwMod) {
                    dispatchCall(argc);
                    break;
                }
                const unsigned int fixed =
                    static_cast<unsigned int>(kwMod->arity());
                if (argc < fixed || (argc - fixed) % 2 != 0) {
                    throwError(&frame, 
                        "VM: keyword-argument fn expects " +
                        std::to_string(fixed) +
                        " positional arguments followed by key/value pairs, got " +
                        std::to_string(argc) + " arguments");
                }
                const unsigned int kvItems = argc - fixed;
                if (kvItems > 0) {
                    const proto::ProtoObject* kwMap = foldKeywordPairs(
                        &frame, stackBase + sp - kvItems, kvItems);
                    sp -= kvItems;
                    pushVal(kwMap);
                    dispatchCall(fixed + 1);
                } else {
                    dispatchCall(fixed);
                }
                break;
            }

            case Op::POP:
                if (sp == 0) throwError(&frame, "VM: POP on empty stack");
                --sp;
                break;

            case Op::RETURN:
                if (sp == 0) return PROTO_NONE;
                return frame.getAutomaticLocal(stackBase + sp - 1);

            case Op::STORE_GLOBAL: {
                if (operand >= mod.constCount())
                    throwError(&frame, "VM: STORE_GLOBAL out-of-range");
                const auto& c = mod.constAt(operand);
                if (c.kind != BytecodeModule::ConstKind::Symbol)
                    throwError(&frame, "VM: STORE_GLOBAL key not a symbol");
                if (sp == 0)
                    throwError(&frame, "VM: STORE_GLOBAL with empty stack");
                const auto* key = reinterpret_cast<const proto::ProtoString*>(c.named);
                const proto::ProtoObject* val =
                    frame.getAutomaticLocal(stackBase + sp - 1);
                const_cast<proto::ProtoObject*>(env.globals)
                    ->setAttribute(&frame, key, val);
                if (c.sval.size() <= 2) noteGlobalStore(&frame, env.globals, c.sval, val);
                break;
            }

            case Op::JUMP:
                ip += operand;
                break;

            case Op::JUMP_IF_FALSE: {
                const proto::ProtoObject* v = popVal();
                if (v == PROTO_NONE || v == PROTO_FALSE) {
                    ip += operand;
                }
                break;
            }

            case Op::LINE: {
                // Debugging only (Opcodes.h): the frame enters a line.
                DebugFrame& df = debugScope.frame();
                if (operand == 0) { df.line = 0; break; }
                df.pc = static_cast<std::size_t>(ip - 1 - mod.code().data());
                if (static_cast<int>(operand) != df.line) {
                    df.line = df.shownLine = static_cast<int>(operand);
                    if (DebugHook* hook = debugHook()) hook->onLine(&frame, &df);
                }
                break;
            }

            case Op::JUMP_BACK:
                ip -= operand;
                // The loop back-edge is protoClojure's garbage-collection
                // safepoint. A `loop` / `recur` is ONE frame, so its context
                // — and with it every cell the loop ever allocated — lives
                // for the loop's whole duration; without this the collector
                // is never shown a loop's garbage and reclaims none of it.
                // Between instructions every live value is in a traced frame
                // slot. See ExecutionEngine.h::gcSafepoint.
                gcSafepoint(&frame);
                break;

            case Op::JUMP_IF_TRUE: {
                const proto::ProtoObject* v = popVal();
                if (v != PROTO_NONE && v != PROTO_FALSE) {
                    ip += operand;
                }
                break;
            }

            case Op::DUP: {
                if (sp == 0) throwError(&frame, "VM: DUP on empty stack");
                pushVal(frame.getAutomaticLocal(stackBase + sp - 1));
                break;
            }

            case Op::MAKE_FN_MULTI: {
                if (operand >= mod.arityGroupCount())
                    throwError(&frame, "VM: MAKE_FN_MULTI out-of-range");
                const auto& group = mod.arityGroup(operand);
                // Tally captures across all arities and slice the stack
                // back into per-arity lists.
                std::size_t totalCaps = 0;
                for (std::size_t bi : group.blockIndices) {
                    totalCaps += static_cast<std::size_t>(
                        mod.block(bi).captureCount());
                }
                if (sp < totalCaps)
                    throwError(&frame, "VM: MAKE_FN_MULTI underflow");

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

                // Immutable, like the single-arity wrapper (MAKE_FN).
                pushVal(env.fnMultiProto->newChild(&frame)
                            ->setAttribute(&frame, env.aritiesKey, aritiesList));
                break;
            }

            case Op::PUSH_NIL:   pushVal(PROTO_NONE);  break;
            case Op::PUSH_TRUE:  pushVal(PROTO_TRUE);  break;
            case Op::PUSH_FALSE: pushVal(PROTO_FALSE); break;

            // Session 11 — SmallInt fast-path binary ops. On the hot path
            // (both args tagged SmallInt, result fits) we inline the
            // arithmetic and skip the ProtoMethod indirection entirely.
            // Any other shape takes the slow path below, which checks that
            // the operands are numbers and computes with protoCore, with
            // the same results and errors as the primitive.
            case Op::ADD:
            case Op::SUB:
            case Op::MUL:
            case Op::LT:
            case Op::LE:
            case Op::GT:
            case Op::GE:
            case Op::EQ: {
                if (sp < 2) throwError(&frame, "VM: binop underflow");
                // The global of this operator was redefined (and the form
                // did not name clojure.core's): call it with the operands,
                // as PUSH_VAR + CALL would (noteGlobalStore).
                if (g_coreOperatorRedefined.load(std::memory_order_relaxed) != 0) [[unlikely]] {
                    // `f` is held by the globals object.
                    if (const proto::ProtoObject* f =
                            redefinedOperator(&frame, env.globals, op, operand)) {
                        // Stack [a b] -> [f a b], then CALL 2.
                        reserveStack(1);
                        const proto::ProtoObject** st = frame.getAutomaticLocals() + stackBase;
                        st[sp] = st[sp - 1];
                        st[sp - 1] = st[sp - 2];
                        st[sp - 2] = f;
                        ++sp;
                        callArgc = 2;
                        goto callDispatch;
                    }
                }
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
                // SmallInt — a float, a LargeInteger, or an operand that is
                // not a number. Arithmetic and ordering take numbers only:
                // protoCore's multiply repeats a string (`(* "ab" 3)`) and
                // its add and compare accept other objects, so anything but
                // a number raises the ClassCastException analogue the
                // primitives raise (throwNotANumber; isNumber is a pointer
                // tag test). Numbers go through protoCore's promoting
                // add/subtract/multiply/compare, which mix SmallInt,
                // LargeInteger and double. EQ is value equality on any
                // operands and shares valuesEqual with the `=` primitive,
                // so maps and vectors compare structurally. The compiler
                // never emits these opcodes when the operator is shadowed
                // by a local. The operands stay rooted in their slots until
                // the result is pushed.
                if (op != Op::EQ &&
                    !(isNumber(&frame, a) && isNumber(&frame, b))) [[unlikely]] {
                    throwNotANumber(&frame, binaryOpName(op),
                                    isNumber(&frame, a) ? b : a);
                }
                sp -= 2;
                const proto::ProtoObject* r = PROTO_NONE;
                switch (op) {
                    case Op::ADD: r = a->add(&frame, b);      break;
                    case Op::SUB: r = a->subtract(&frame, b); break;
                    case Op::MUL: r = a->multiply(&frame, b); break;
                    // IEEE ordering (protoCore partialCompare): exact across
                    // SmallInteger, LargeInteger and double, and every
                    // comparison with NaN is false.
                    case Op::LT:  r = a->partialCompare(&frame, b) <  0 ? PROTO_TRUE : PROTO_FALSE; break;
                    case Op::LE:  r = a->partialCompare(&frame, b) <= 0 ? PROTO_TRUE : PROTO_FALSE; break;
                    case Op::GT:  r = a->partialCompare(&frame, b) >  0 ? PROTO_TRUE : PROTO_FALSE; break;
                    case Op::GE:  r = a->partialCompare(&frame, b) >= 0 ? PROTO_TRUE : PROTO_FALSE; break;
                    case Op::EQ:
                        r = valuesEqual(&frame, a, b)
                                ? PROTO_TRUE : PROTO_FALSE;
                        break;
                    default: break;
                }
                pushVal(r);
                break;
            }

            case Op::TRY_BEGIN:
                // Emitting TRY_BEGIN marks the module (hasHandlers), so only
                // a handler frame ever sees one.
                if constexpr (kHandlers) {
                    pushTryHandler(handlers, ip + operand, sp);
                } else {
                    throwError(&frame, "VM: TRY_BEGIN in a frame without handlers");
                }
                break;

            case Op::TRY_END:
                if (handlers.empty())
                    throwError(&frame, "VM: TRY_END without a handler");
                handlers.pop_back();
                break;

            case Op::THROW:
                // The value stays in its slot, rooted, until the
                // ProtoException has pinned it.
                if (sp == 0) throwError(&frame, "VM: THROW on empty stack");
                throwValue(&frame, peekAt(0));

            case Op::STORE_CAUGHT:
                if constexpr (kHandlers) {
                    storeCaught(caught, justCaught, operand);
                    frame.setAutomaticLocal(locBase + operand, popVal());
                } else {
                    throwError(&frame, "VM: STORE_CAUGHT in a frame without handlers");
                }
                break;

            case Op::RETHROW:
                // Outside any C++ catch clause: rethrow_exception starts the
                // search here, as a `throw;` in the handler would.
                if (operand < caught.size() && caught[operand])
                    std::rethrow_exception(caught[operand]);
                // A translated C++ error: raise its value.
                rethrowValue(&frame, frame.getAutomaticLocal(locBase + operand));

            case Op::EXC_MATCH: {
                const proto::ProtoObject* v = popVal();
                pushVal(exceptionMatches(&frame, v, static_cast<int>(operand))
                        ? PROTO_TRUE : PROTO_FALSE);
                break;
            }

            case Op::EXC_MATCH_TYPE: {
                // Both stay in their slots, rooted, during the test.
                const bool match = matchCatchType(&frame, peekAt(1), peekAt(0),
                                                  mod.constAt(operand).sval);
                sp -= 2;
                pushVal(match ? PROTO_TRUE : PROTO_FALSE);
                break;
            }

            case Op::RESOLVE_CLASS:
                pushVal(resolveCatchType(&frame, env.globals, mod.constAt(operand).sval));
                break;

            case Op::GET_MEMBER: {
                const auto& c = mod.constAt(operand);
                const proto::ProtoObject* v = getMember(&frame, peekAt(0),
                    reinterpret_cast<const proto::ProtoString*>(c.named), c.sval);
                --sp;
                pushVal(v);
                break;
            }

            case Op::CALL_MEMBER: {
                // The data word: a NOP carrying the member's symbol index.
                const auto& c = mod.constAt(*ip++ >> kOperandShift);
                const proto::ProtoObject* result = callMember(&frame, env.engine,
                    peekAt(operand), reinterpret_cast<const proto::ProtoString*>(c.named),
                    c.sval, frame.getAutomaticLocals() + stackBase + sp - operand, operand);
                sp -= (operand + 1);
                pushVal(result);
                break;
            }

            default:
                throwError(&frame, "VM: unknown opcode");
        }
    }
    return (sp == 0) ? PROTO_NONE : frame.getAutomaticLocal(stackBase + sp - 1);
    };

    // Root the result before `frame` is destroyed (see execute): in the
    // caller's slot when it gave one, else through frame.returnValue.
    auto anchor = [&](const proto::ProtoObject* r) {
        if (resultSlot) *resultSlot = r;
        else frame.returnValue = r;
        return r;
    };
    if constexpr (!kHandlers) {
        return anchor(dispatchLoop());
    } else {
        // An exception escaping an instruction — raised here, by a primitive
        // or by a nested call, whose frames it has already unwound — lands in
        // the catch below. With no active handler it leaves the frame.
        // Otherwise the innermost handler takes it: the operand stack is cut
        // back to the handler's depth, the exception value is pushed (which
        // roots it in this frame; protoCore pins a ProtoException's value
        // until then) and the loop resumes at the handler code.
        //
        // With no handler the exception is re-thrown AFTER the catch clause
        // has completed (std::rethrow_exception), never with `throw;` inside
        // it. Under the Itanium ABI (Linux, macOS) the two are the same. Under
        // MSVC a catch clause runs before the stack below it is released, so a
        // `throw;` inside it would start the next search below the frames
        // already left behind, and a StackOverflowError raised at depth would
        // overflow the native stack while propagating.
        for (;;) {
            std::exception_ptr passOn;
            try {
                return anchor(dispatchLoop());
            } catch (const proto::ProtoException& thrown) {
                // Every language's exception: the handler code selects by
                // the value's prototype chain (EXC_MATCH, EXC_MATCH_TYPE)
                // and rethrows what no clause handles (RETHROW).
                if (handlers.empty()) {
                    passOn = std::current_exception();
                } else {
                    const TryHandler h = handlers.back();
                    handlers.pop_back();
                    sp = h.sp;
                    pushVal(thrown.value());
                    justCaught = std::current_exception();
                    ip = h.target;
                }
            } catch (const std::exception& error) {
                // A foreign C++ exception (a module defect) or a
                // StackOverflowError: a classed exception from its what().
                if (handlers.empty()) {
                    passOn = std::current_exception();
                } else {
                    const TryHandler h = handlers.back();
                    handlers.pop_back();
                    sp = h.sp;
                    pushVal(exceptionFromError(&frame, error));
                    justCaught = nullptr;   // RETHROW raises the value
                    ip = h.target;
                }
            }
            if (passOn) std::rethrow_exception(passOn);
        }
    }
}

// ---------------------------------------------------------------------------
// The debugger's question: does a frame catch this? (DebugHook.h)
// ---------------------------------------------------------------------------

namespace {

// The next instruction from `ip` that is not a line mark or a NOP.
const Instr* skipMarks(const Instr* ip, const Instr* end) {
    while (ip < end) {
        const Op op = static_cast<Op>(*ip & 0xFF);
        if (op != Op::LINE && op != Op::NOP) break;
        ++ip;
    }
    return ip;
}

// Whether the handler code at `target` (compileTry's layout: STORE_CAUGHT ex,
// then per clause PUSH_LOCAL ex, a type test, JUMP_IF_FALSE next, ...,
// RETHROW ex) has a clause selecting `v`. A finally handler (STORE_CAUGHT,
// the finally forms, RETHROW) selects nothing. A shape it does not know, or
// a type it cannot work out, counts as catching.
bool handlerCatches(proto::ProtoContext* ctx, const DebugFrame& f, const Instr* target,
                    const proto::ProtoObject* v) {
    const BytecodeModule& mod = *f.mod;
    const Instr* const end = mod.code().data() + mod.code().size();
    auto opAt = [](const Instr* p) { return static_cast<Op>(*p & 0xFF); };
    auto operandAt = [](const Instr* p) { return static_cast<std::uint32_t>(*p >> kOperandShift); };
    auto isPushLocal = [&](const Instr* p) {
        return opAt(p) == Op::PUSH_LOCAL || opAt(p) == Op::PUSH_LOCAL_CLEAR;
    };

    const Instr* ip = skipMarks(target, end);
    if (ip >= end || opAt(ip) != Op::STORE_CAUGHT) return true;
    const std::uint32_t ex = operandAt(ip++);
    const ActiveCallContext* cc = activeCallContext();
    for (int clauses = 0; clauses < 4096; ++clauses) {
        ip = skipMarks(ip, end);
        if (ip >= end) return true;
        if (opAt(ip) == Op::RETHROW) return false;               // no clause selected it
        if (!isPushLocal(ip) || operandAt(ip) != ex)  // a finally's forms
            return false;
        ++ip;
        bool match = true;
        if (ip < end && opAt(ip) == Op::EXC_MATCH) {
            match = exceptionMatches(ctx, v, static_cast<int>(operandAt(ip)));
            ++ip;
        } else {
            const proto::ProtoObject* type = nullptr;
            if (ip < end && opAt(ip) == Op::RESOLVE_CLASS) {
                if (!cc) return true;
                type = resolveCatchType(ctx, cc->globals, mod.constAt(operandAt(ip)).sval);
                ++ip;
            } else if (ip < end && isPushLocal(ip)) {
                type = f.ctx->getAutomaticLocal(operandAt(ip));
                ++ip;
                if (ip < end && opAt(ip) == Op::GET_MEMBER) {
                    const auto& c = mod.constAt(operandAt(ip));
                    const proto::ProtoObject* member = nullptr;
                    if (!readMember(ctx, type, reinterpret_cast<const proto::ProtoString*>(c.named), member))
                        return true;   // the clause itself raises
                    type = member;
                    ++ip;
                }
            } else {
                return true;
            }
            if (ip >= end || opAt(ip) != Op::EXC_MATCH_TYPE) return true;
            ++ip;
            if (!isForeignObject(ctx, type) && !isException(ctx, type)) return true;  // raises
            match = valueIsInstanceOf(ctx, v, type);
        }
        if (ip >= end || opAt(ip) != Op::JUMP_IF_FALSE) return true;
        if (match) return true;
        ip = ip + 1 + operandAt(ip);                             // the next clause
    }
    return true;
}

} // namespace

bool debugFrameCatches(proto::ProtoContext* ctx, const DebugFrame* f,
                       const proto::ProtoObject* value) {
    if (!f || !f->handlers || !f->mod) return false;
    const auto& handlers = *static_cast<const std::vector<TryHandler>*>(f->handlers);
    if (handlers.empty()) return false;
    DebugQuietScope quiet;
    // The type lookups may allocate: the value stays rooted in this scope.
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(1);
    scope.setAutomaticLocal(0, value);
    try {
        for (auto it = handlers.rbegin(); it != handlers.rend(); ++it)
            if (handlerCatches(&scope, *f, it->target, value)) return true;
    } catch (...) {
        return true;   // a type lookup failed: the clause would raise, not pass it on
    }
    return false;
}

template const proto::ProtoObject* ExecutionEngine::executeFrame<false>(
    proto::ProtoContext*, const BytecodeModule&, const ActiveCallContext&,
    const proto::ProtoObject* const*, unsigned int, const proto::ProtoObject*,
    const proto::ProtoObject* const*, unsigned int, const proto::ProtoObject*,
    const proto::ProtoObject**);
template const proto::ProtoObject* ExecutionEngine::executeFrame<true>(
    proto::ProtoContext*, const BytecodeModule&, const ActiveCallContext&,
    const proto::ProtoObject* const*, unsigned int, const proto::ProtoObject*,
    const proto::ProtoObject* const*, unsigned int, const proto::ProtoObject*,
    const proto::ProtoObject**);

} // namespace protoClojure
