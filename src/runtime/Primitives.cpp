#include "Primitives.h"
#include "ExecutionEngine.h"

#include "protoCore.h"

#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <string>

namespace protoClojure {

// Session 7 — thread-local active-call context. ExecutionEngine::run
// installs it on entry, restores on exit. Primitives that need to invoke
// user callables (map / reduce / filter) read this slot.
namespace {
thread_local ActiveCallContext g_active{};
thread_local bool              g_activeSet = false;
}

void setActiveCallContext(const ActiveCallContext& cc) {
    g_active = cc;
    g_activeSet = true;
}

void clearActiveCallContext() {
    g_activeSet = false;
}

const ActiveCallContext* activeCallContext() {
    return g_activeSet ? &g_active : nullptr;
}

namespace {

// Common helpers ------------------------------------------------------------

// Pull an argument and coerce to long long. Pre-session-9 helper kept for
// the comparison-chain template; for arithmetic see argAsNumber below.
long long argAsLong(proto::ProtoContext* ctx, const proto::ProtoList* args,
                    int i, const char* primName) {
    const proto::ProtoObject* a = args->getAt(ctx, i);
    if (!a || !a->isInteger(ctx)) {
        throw std::runtime_error(
            std::string(primName) + ": argument " + std::to_string(i) +
            " is not an integer");
    }
    return a->asLong(ctx);
}

// Session 9 — numeric union. Carries either an int or a float, with the
// promotion rule "any float in the input → float result". Used by the
// arithmetic and comparison primitives.
struct Num {
    bool   isFloat;
    long long  ival;
    double     dval;
    double  asDouble() const { return isFloat ? dval : static_cast<double>(ival); }
};

Num argAsNumber(proto::ProtoContext* ctx, const proto::ProtoList* args,
                int i, const char* primName) {
    const proto::ProtoObject* a = args->getAt(ctx, i);
    if (!a) {
        throw std::runtime_error(
            std::string(primName) + ": argument " + std::to_string(i) + " is nil");
    }
    if (a->isInteger(ctx)) return {false, a->asLong(ctx), 0.0};
    if (a->isFloat(ctx))   return {true,  0, a->asDouble(ctx)};
    throw std::runtime_error(
        std::string(primName) + ": argument " + std::to_string(i) +
        " is not a number");
}

// Pointer-tag constants matching protoCore's internal layout. Mirrored
// from headers/proto_internal.h. Used to tell a genuine list (large or
// small inline form) apart from a string (which would otherwise look
// list-shaped to asList(), which exposes the char-list view).
constexpr unsigned int kTagList      = 2;
constexpr unsigned int kTagListSmall = 25;

bool isListTag(const proto::ProtoObject* v) {
    if (!v) return false;
    unsigned int t =
        static_cast<unsigned int>(reinterpret_cast<uintptr_t>(v) & 0x3F);
    return t == kTagList || t == kTagListSmall;
}

// Print a single value in `println` / `str` form. No surrounding quotes on
// strings (that is `pr`'s job, not `println`'s).
//
// Note: at v0.0.x runtime, the VM materialises String const-pool entries
// via ctx->fromUTF8String, which returns a plain ProtoString (NOT the
// Reader's wrapper — that wrapping only lives between Reader and Compiler).
// So at runtime, strings are just stringy ProtoObjects and printValue can
// reach the bytes directly.
void printValue(proto::ProtoContext* ctx, std::FILE* out,
                const proto::ProtoObject* v) {
    if (!v || v == PROTO_NONE) { std::fputs("nil", out); return; }
    if (v == PROTO_TRUE)        { std::fputs("true", out); return; }
    if (v == PROTO_FALSE)       { std::fputs("false", out); return; }
    if (v->isInteger(ctx)) {
        // SmallInt: print via the tagged-pointer fast extract. LargeInt:
        // route through protoCore's asIntegerString (long-long range may
        // overflow). Mirror of the tagged check in ExecutionEngine.cpp;
        // duplicated here to keep this file independent of the VM.
        constexpr unsigned long kSmallIntMask  = 0x3FFUL;
        constexpr unsigned long kSmallIntValue = 0x001UL;
        unsigned long bits = reinterpret_cast<unsigned long>(v);
        if ((bits & kSmallIntMask) == kSmallIntValue) {
            std::fprintf(out, "%lld",
                static_cast<long long>(reinterpret_cast<long long>(v) >> 10));
        } else {
            const proto::ProtoString* s = v->asIntegerString(ctx);
            std::fputs(s->toStdString(ctx).c_str(), out);
        }
        return;
    }
    if (v->isFloat(ctx)) {
        double d = v->asDouble(ctx);
        // Match Clojure-JVM: integer-valued floats print with `.0`.
        if (d == static_cast<long long>(d)) {
            std::fprintf(out, "%lld.0", static_cast<long long>(d));
        } else {
            std::fprintf(out, "%g", d);
        }
        return;
    }
    if (isListTag(v)) {
        const proto::ProtoList* lst = v->asList(ctx);
        std::fputc('(', out);
        unsigned long n = lst->getSize(ctx);
        for (unsigned long i = 0; i < n; ++i) {
            if (i > 0) std::fputc(' ', out);
            printValue(ctx, out, lst->getAt(ctx, static_cast<int>(i)));
        }
        std::fputc(')', out);
        return;
    }
    if (v->isTuple(ctx)) {
        const proto::ProtoTuple* t =
            reinterpret_cast<const proto::ProtoTuple*>(v);
        std::fputc('[', out);
        unsigned long n = t->getSize(ctx);
        for (unsigned long i = 0; i < n; ++i) {
            if (i > 0) std::fputc(' ', out);
            printValue(ctx, out, t->getAt(ctx, static_cast<int>(i)));
        }
        std::fputc(']', out);
        return;
    }
    if (proto::ProtoObject::isStringTagFast(v)) {
        const proto::ProtoString* s =
            reinterpret_cast<const proto::ProtoString*>(v);
        std::fputs(s->toStdString(ctx).c_str(), out);
        return;
    }
    // Session 13 — print maps. We check via ActiveCallContext rather
    // than carrying mapMarkerProto through printValue everywhere.
    const ActiveCallContext* cc = activeCallContext();
    if (cc && v->getPrototype(ctx) == cc->mapMarkerProto) {
        const proto::ProtoObject* eRaw =
            v->getAttribute(ctx, cc->entriesKey);
        const proto::ProtoSparseList* sparse = eRaw
            ? reinterpret_cast<const proto::ProtoSparseList*>(eRaw)
            : ctx->newSparseList();
        std::fputc('{', out);
        struct Acc { proto::ProtoContext* ctx; std::FILE* out; bool first; };
        Acc acc{ctx, out, true};
        sparse->processElements(ctx, &acc,
            [](proto::ProtoContext* c, void* self, unsigned long /*hash*/,
               const proto::ProtoObject* bucketObj) {
                auto* a = static_cast<Acc*>(self);
                if (!bucketObj) return;
                const proto::ProtoList* bucket = bucketObj->asList(c);
                unsigned long n = bucket->getSize(c);
                for (unsigned long i = 0; i < n; i += 2) {
                    if (!a->first) std::fputs(", ", a->out);
                    a->first = false;
                    printValue(c, a->out, bucket->getAt(c, (int)i));
                    std::fputc(' ', a->out);
                    printValue(c, a->out, bucket->getAt(c, (int)(i + 1)));
                }
            });
        std::fputc('}', out);
        return;
    }
    std::fputs("#<unprintable>", out);
}

void appendValue(proto::ProtoContext* ctx, std::ostringstream& os,
                 const proto::ProtoObject* v) {
    if (!v || v == PROTO_NONE) { os << "nil"; return; }
    if (v == PROTO_TRUE)        { os << "true"; return; }
    if (v == PROTO_FALSE)       { os << "false"; return; }
    if (v->isInteger(ctx)) {
        constexpr unsigned long kSmallIntMask  = 0x3FFUL;
        constexpr unsigned long kSmallIntValue = 0x001UL;
        unsigned long bits = reinterpret_cast<unsigned long>(v);
        if ((bits & kSmallIntMask) == kSmallIntValue) {
            os << static_cast<long long>(reinterpret_cast<long long>(v) >> 10);
        } else {
            os << v->asIntegerString(ctx)->toStdString(ctx);
        }
        return;
    }
    if (v->isFloat(ctx)) {
        double d = v->asDouble(ctx);
        if (d == static_cast<long long>(d)) {
            os << static_cast<long long>(d) << ".0";
        } else {
            os << d;
        }
        return;
    }
    if (isListTag(v)) {
        const proto::ProtoList* lst = v->asList(ctx);
        os << '(';
        unsigned long n = lst->getSize(ctx);
        for (unsigned long i = 0; i < n; ++i) {
            if (i > 0) os << ' ';
            appendValue(ctx, os, lst->getAt(ctx, static_cast<int>(i)));
        }
        os << ')';
        return;
    }
    if (v->isTuple(ctx)) {
        const proto::ProtoTuple* t =
            reinterpret_cast<const proto::ProtoTuple*>(v);
        os << '[';
        unsigned long n = t->getSize(ctx);
        for (unsigned long i = 0; i < n; ++i) {
            if (i > 0) os << ' ';
            appendValue(ctx, os, t->getAt(ctx, static_cast<int>(i)));
        }
        os << ']';
        return;
    }
    if (proto::ProtoObject::isStringTagFast(v)) {
        const proto::ProtoString* s =
            reinterpret_cast<const proto::ProtoString*>(v);
        os << s->toStdString(ctx);
        return;
    }
    os << "#<unprintable>";
}

// println — print each positional arg, space-separated, followed by \n.
// Returns nil (PROTO_NONE) per Clojure-JVM semantics.
//
// P1: walks the positional-args ProtoList by index, immediately consuming
// each element. No intermediate ProtoObject* held across an allocation
// that we can avoid. The println'd objects are read-only inspected for
// their string representation.
//
// P3: writes to stdout via std::printf. No std container is stored inside
// protoCore; the std::string we build via toStdString lives transiently
// on the C++ stack.
const proto::ProtoObject* prim_println(proto::ProtoContext* ctx,
                                       const proto::ProtoObject* /*self*/,
                                       const proto::ParentLink* /*parents*/,
                                       const proto::ProtoList* args,
                                       const proto::ProtoSparseList* /*kwargs*/) {
    unsigned long n = args ? args->getSize(ctx) : 0;
    for (unsigned long i = 0; i < n; ++i) {
        if (i > 0) std::fputc(' ', stdout);
        printValue(ctx, stdout, args->getAt(ctx, static_cast<int>(i)));
    }
    std::fputc('\n', stdout);
    std::fflush(stdout);
    return PROTO_NONE;
}

// Arithmetic (integer-only in v0.0.x) -------------------------------------

// Arithmetic helper: walk all args; if any is float, the result is a
// double computed via `dop`; otherwise it's a long computed via `iop`.
template <typename Iop, typename Dop>
const proto::ProtoObject* arithFold(proto::ProtoContext* ctx,
                                    const proto::ProtoList* args,
                                    const char* name,
                                    long long iIdent, double dIdent,
                                    Iop iop, Dop dop) {
    unsigned long n = args ? args->getSize(ctx) : 0;
    bool anyFloat = false;
    for (unsigned long i = 0; i < n; ++i) {
        Num x = argAsNumber(ctx, args, (int)i, name);
        if (x.isFloat) { anyFloat = true; break; }
    }
    if (anyFloat) {
        double acc = dIdent;
        bool started = false;
        for (unsigned long i = 0; i < n; ++i) {
            double v = argAsNumber(ctx, args, (int)i, name).asDouble();
            acc = started ? dop(acc, v) : v;
            started = true;
        }
        return ctx->fromDouble(n == 0 ? dIdent : acc);
    } else {
        long long acc = iIdent;
        bool started = false;
        for (unsigned long i = 0; i < n; ++i) {
            long long v = argAsLong(ctx, args, (int)i, name);
            acc = started ? iop(acc, v) : v;
            started = true;
        }
        return ctx->fromLong(n == 0 ? iIdent : acc);
    }
}

const proto::ProtoObject* prim_plus(proto::ProtoContext* ctx,
                                    const proto::ProtoObject*,
                                    const proto::ParentLink*,
                                    const proto::ProtoList* args,
                                    const proto::ProtoSparseList*) {
    unsigned long n = args ? args->getSize(ctx) : 0;
    if (n == 0) return ctx->fromLong(0);
    bool anyFloat = false;
    for (unsigned long i = 0; i < n; ++i) {
        if (argAsNumber(ctx, args, (int)i, "+").isFloat) { anyFloat = true; break; }
    }
    if (anyFloat) {
        double acc = 0.0;
        for (unsigned long i = 0; i < n; ++i)
            acc += argAsNumber(ctx, args, (int)i, "+").asDouble();
        return ctx->fromDouble(acc);
    }
    long long acc = 0;
    for (unsigned long i = 0; i < n; ++i) acc += argAsLong(ctx, args, (int)i, "+");
    return ctx->fromLong(acc);
}

const proto::ProtoObject* prim_minus(proto::ProtoContext* ctx,
                                     const proto::ProtoObject*,
                                     const proto::ParentLink*,
                                     const proto::ProtoList* args,
                                     const proto::ProtoSparseList*) {
    unsigned long n = args ? args->getSize(ctx) : 0;
    if (n == 0) throw std::runtime_error("- : needs at least one arg");
    bool anyFloat = false;
    for (unsigned long i = 0; i < n; ++i) {
        if (argAsNumber(ctx, args, (int)i, "-").isFloat) { anyFloat = true; break; }
    }
    if (anyFloat) {
        if (n == 1) return ctx->fromDouble(-argAsNumber(ctx, args, 0, "-").asDouble());
        double acc = argAsNumber(ctx, args, 0, "-").asDouble();
        for (unsigned long i = 1; i < n; ++i)
            acc -= argAsNumber(ctx, args, (int)i, "-").asDouble();
        return ctx->fromDouble(acc);
    }
    if (n == 1) return ctx->fromLong(-argAsLong(ctx, args, 0, "-"));
    long long acc = argAsLong(ctx, args, 0, "-");
    for (unsigned long i = 1; i < n; ++i) acc -= argAsLong(ctx, args, (int)i, "-");
    return ctx->fromLong(acc);
}

const proto::ProtoObject* prim_mul(proto::ProtoContext* ctx,
                                   const proto::ProtoObject*,
                                   const proto::ParentLink*,
                                   const proto::ProtoList* args,
                                   const proto::ProtoSparseList*) {
    unsigned long n = args ? args->getSize(ctx) : 0;
    if (n == 0) return ctx->fromLong(1);
    bool anyFloat = false;
    for (unsigned long i = 0; i < n; ++i) {
        if (argAsNumber(ctx, args, (int)i, "*").isFloat) { anyFloat = true; break; }
    }
    if (anyFloat) {
        double acc = 1.0;
        for (unsigned long i = 0; i < n; ++i)
            acc *= argAsNumber(ctx, args, (int)i, "*").asDouble();
        return ctx->fromDouble(acc);
    }
    long long acc = 1;
    for (unsigned long i = 0; i < n; ++i) acc *= argAsLong(ctx, args, (int)i, "*");
    return ctx->fromLong(acc);
}

// `/` arrives in session 9 — float-by-default once any arg is float; int
// truncating div when all ints. Matches Clojure for ints when divisible;
// not bit-for-bit for Ratios (we have no Ratio in v0.9, so we truncate).
const proto::ProtoObject* prim_div(proto::ProtoContext* ctx,
                                   const proto::ProtoObject*,
                                   const proto::ParentLink*,
                                   const proto::ProtoList* args,
                                   const proto::ProtoSparseList*) {
    unsigned long n = args ? args->getSize(ctx) : 0;
    if (n == 0) throw std::runtime_error("/: needs at least one arg");
    bool anyFloat = false;
    for (unsigned long i = 0; i < n; ++i) {
        if (argAsNumber(ctx, args, (int)i, "/").isFloat) { anyFloat = true; break; }
    }
    if (anyFloat) {
        if (n == 1) return ctx->fromDouble(1.0 / argAsNumber(ctx, args, 0, "/").asDouble());
        double acc = argAsNumber(ctx, args, 0, "/").asDouble();
        for (unsigned long i = 1; i < n; ++i) {
            double v = argAsNumber(ctx, args, (int)i, "/").asDouble();
            if (v == 0.0) throw std::runtime_error("/: divide by zero");
            acc /= v;
        }
        return ctx->fromDouble(acc);
    }
    if (n == 1) {
        long long v = argAsLong(ctx, args, 0, "/");
        if (v == 0) throw std::runtime_error("/: divide by zero");
        return ctx->fromLong(1 / v);
    }
    long long acc = argAsLong(ctx, args, 0, "/");
    for (unsigned long i = 1; i < n; ++i) {
        long long v = argAsLong(ctx, args, (int)i, "/");
        if (v == 0) throw std::runtime_error("/: divide by zero");
        acc /= v;
    }
    return ctx->fromLong(acc);
}

const proto::ProtoObject* prim_inc(proto::ProtoContext* ctx,
                                   const proto::ProtoObject*,
                                   const proto::ParentLink*,
                                   const proto::ProtoList* args,
                                   const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 1)
        throw std::runtime_error("inc: expects 1 arg");
    Num x = argAsNumber(ctx, args, 0, "inc");
    if (x.isFloat) return ctx->fromDouble(x.dval + 1.0);
    return ctx->fromLong(x.ival + 1);
}

const proto::ProtoObject* prim_dec(proto::ProtoContext* ctx,
                                   const proto::ProtoObject*,
                                   const proto::ParentLink*,
                                   const proto::ProtoList* args,
                                   const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 1)
        throw std::runtime_error("dec: expects 1 arg");
    Num x = argAsNumber(ctx, args, 0, "dec");
    if (x.isFloat) return ctx->fromDouble(x.dval - 1.0);
    return ctx->fromLong(x.ival - 1);
}

// Comparison ---------------------------------------------------------------

// Variadic monotonic chain helper. Returns true if for every adjacent pair
// (a, b) the predicate(a, b) holds. Numeric promotion: any float in the
// chain promotes ALL comparisons to double — matches Clojure-JVM.
template <typename Pred>
const proto::ProtoObject* monotonicChain(proto::ProtoContext* ctx,
                                         const proto::ProtoList* args,
                                         const char* name,
                                         Pred pred) {
    unsigned long n = args ? args->getSize(ctx) : 0;
    if (n < 2) return PROTO_TRUE;            // 0- or 1-arg form is true
    double prev = argAsNumber(ctx, args, 0, name).asDouble();
    for (unsigned long i = 1; i < n; ++i) {
        double cur = argAsNumber(ctx, args, (int)i, name).asDouble();
        if (!pred(prev, cur)) return PROTO_FALSE;
        prev = cur;
    }
    return PROTO_TRUE;
}

const proto::ProtoObject* prim_lt(proto::ProtoContext* ctx,
                                  const proto::ProtoObject*,
                                  const proto::ParentLink*,
                                  const proto::ProtoList* args,
                                  const proto::ProtoSparseList*) {
    return monotonicChain(ctx, args, "<", [](double a, double b){ return a < b; });
}

const proto::ProtoObject* prim_le(proto::ProtoContext* ctx,
                                  const proto::ProtoObject*,
                                  const proto::ParentLink*,
                                  const proto::ProtoList* args,
                                  const proto::ProtoSparseList*) {
    return monotonicChain(ctx, args, "<=", [](double a, double b){ return a <= b; });
}

const proto::ProtoObject* prim_gt(proto::ProtoContext* ctx,
                                  const proto::ProtoObject*,
                                  const proto::ParentLink*,
                                  const proto::ProtoList* args,
                                  const proto::ProtoSparseList*) {
    return monotonicChain(ctx, args, ">", [](double a, double b){ return a > b; });
}

const proto::ProtoObject* prim_ge(proto::ProtoContext* ctx,
                                  const proto::ProtoObject*,
                                  const proto::ParentLink*,
                                  const proto::ProtoList* args,
                                  const proto::ProtoSparseList*) {
    return monotonicChain(ctx, args, ">=", [](double a, double b){ return a >= b; });
}

const proto::ProtoObject* prim_eq(proto::ProtoContext* ctx,
                                  const proto::ProtoObject*,
                                  const proto::ParentLink*,
                                  const proto::ProtoList* args,
                                  const proto::ProtoSparseList*) {
    // = on integers in v0.0.x — for collections / strings we need structural
    // equality which lands later. Variadic chain like Clojure's =.
    return monotonicChain(ctx, args, "=", [](double a, double b){ return a == b; });
}

// str ----------------------------------------------------------------------

// (str x y z) → concatenated print-string of all args. Used both for
// printing-with-formatting and for building messages.
const proto::ProtoObject* prim_str(proto::ProtoContext* ctx,
                                   const proto::ProtoObject*,
                                   const proto::ParentLink*,
                                   const proto::ProtoList* args,
                                   const proto::ProtoSparseList*) {
    unsigned long n = args ? args->getSize(ctx) : 0;
    std::ostringstream os;
    for (unsigned long i = 0; i < n; ++i) {
        appendValue(ctx, os, args->getAt(ctx, (int)i));
    }
    return ctx->fromUTF8String(os.str().c_str());
}

// List + predicate primitives (session 7) -----------------------------------

// Treat nil as the empty seq. Vectors (ProtoTuple) are seq-coerced via
// ProtoTuple::asList, which converts in O(N) but is fine for v0.9
// benchmarks (consumers walk linearly anyway). Strings stay opaque for
// session 9 — supported in session 10.
const proto::ProtoList* asSeqOrNull(proto::ProtoContext* ctx,
                                    const proto::ProtoObject* v) {
    if (!v || v == PROTO_NONE) return nullptr;
    if (isListTag(v)) return v->asList(ctx);
    if (v->isTuple(ctx)) {
        // v->asTuple() not exposed by handle API; route via the tuple
        // protocol on the object.
        const proto::ProtoTuple* t =
            reinterpret_cast<const proto::ProtoTuple*>(v);
        return t->asList(ctx);
    }
    throw std::runtime_error(
        "seq op: argument is not a list or vector");
}

// Returns the ProtoTuple* underlying `v` if it's a vector; nullptr otherwise.
const proto::ProtoTuple* asTupleOrNull(proto::ProtoContext* ctx,
                                       const proto::ProtoObject* v) {
    if (!v) return nullptr;
    if (!v->isTuple(ctx)) return nullptr;
    return reinterpret_cast<const proto::ProtoTuple*>(v);
}

const proto::ProtoObject* prim_list(proto::ProtoContext* ctx,
                                    const proto::ProtoObject*,
                                    const proto::ParentLink*,
                                    const proto::ProtoList* args,
                                    const proto::ProtoSparseList*) {
    const proto::ProtoObject* out = ctx->newList()->asObject(ctx);
    unsigned long n = args ? args->getSize(ctx) : 0;
    for (unsigned long i = 0; i < n; ++i) {
        out = out->asList(ctx)
            ->appendLast(ctx, args->getAt(ctx, static_cast<int>(i)))
            ->asObject(ctx);
    }
    return out;
}

// Session 9 — `(vector x y z)` builds a ProtoTuple. The bytecode form
// `[x y z]` desugars to a call here, so vector-literal performance is
// dominated by tuple build cost (O(N)).
const proto::ProtoObject* prim_vector(proto::ProtoContext* ctx,
                                      const proto::ProtoObject*,
                                      const proto::ParentLink*,
                                      const proto::ProtoList* args,
                                      const proto::ProtoSparseList*) {
    unsigned long n = args ? args->getSize(ctx) : 0;
    // Build via newList → appendLast → asTuple-equivalent path.
    // The cheapest path is `newTupleFromList(list)` once we have the list.
    const proto::ProtoObject* lstObj = ctx->newList()->asObject(ctx);
    for (unsigned long i = 0; i < n; ++i) {
        lstObj = lstObj->asList(ctx)
            ->appendLast(ctx, args->getAt(ctx, static_cast<int>(i)))
            ->asObject(ctx);
    }
    return ctx->newTupleFromList(lstObj->asList(ctx))->asObject(ctx);
}

// (vec coll) — converts any seqable (list or vector) to a vector.
const proto::ProtoObject* prim_vec(proto::ProtoContext* ctx,
                                   const proto::ProtoObject*,
                                   const proto::ParentLink*,
                                   const proto::ProtoList* args,
                                   const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 1)
        throw std::runtime_error("vec: expects 1 arg");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    if (v->isTuple(ctx)) return v;  // already a vector
    const proto::ProtoList* lst = asSeqOrNull(ctx, v);
    if (!lst) return ctx->newTuple()->asObject(ctx);
    return ctx->newTupleFromList(lst)->asObject(ctx);
}

// (nth coll i) / (nth coll i not-found)
const proto::ProtoObject* prim_nth(proto::ProtoContext* ctx,
                                   const proto::ProtoObject*,
                                   const proto::ParentLink*,
                                   const proto::ProtoList* args,
                                   const proto::ProtoSparseList*) {
    unsigned long ac = args ? args->getSize(ctx) : 0;
    if (ac != 2 && ac != 3)
        throw std::runtime_error("nth: expects (nth coll i) or (nth coll i nf)");
    const proto::ProtoObject* coll = args->getAt(ctx, 0);
    long long idx = argAsLong(ctx, args, 1, "nth");
    const proto::ProtoObject* notFound = (ac == 3) ? args->getAt(ctx, 2) : nullptr;

    // Vector path: O(log N).
    if (coll && coll->isTuple(ctx)) {
        const proto::ProtoTuple* t =
            reinterpret_cast<const proto::ProtoTuple*>(coll);
        long long sz = static_cast<long long>(t->getSize(ctx));
        if (idx < 0 || idx >= sz) {
            if (notFound) return notFound;
            throw std::runtime_error("nth: index out of bounds");
        }
        return t->getAt(ctx, static_cast<int>(idx));
    }
    // List path: O(N).
    const proto::ProtoList* lst = asSeqOrNull(ctx, coll);
    if (!lst) {
        if (notFound) return notFound;
        throw std::runtime_error("nth: nil collection");
    }
    long long sz = static_cast<long long>(lst->getSize(ctx));
    if (idx < 0 || idx >= sz) {
        if (notFound) return notFound;
        throw std::runtime_error("nth: index out of bounds");
    }
    return lst->getAt(ctx, static_cast<int>(idx));
}

const proto::ProtoObject* prim_vector_p(proto::ProtoContext* ctx,
                                        const proto::ProtoObject*,
                                        const proto::ParentLink*,
                                        const proto::ProtoList* args,
                                        const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 1)
        throw std::runtime_error("vector?: expects 1 arg");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    return (v && v->isTuple(ctx)) ? PROTO_TRUE : PROTO_FALSE;
}

const proto::ProtoObject* prim_list_p(proto::ProtoContext* ctx,
                                      const proto::ProtoObject*,
                                      const proto::ParentLink*,
                                      const proto::ProtoList* args,
                                      const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 1)
        throw std::runtime_error("list?: expects 1 arg");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    return (v && isListTag(v)) ? PROTO_TRUE : PROTO_FALSE;
}

// Session 13 — map primitives. Wire shape: a map is a child of
// `mapMarkerProto` with `__entries__` = a protoCore `ProtoSparseList`
// indexed by `key->getHash(ctx)`. Each slot stores a small "bucket"
// `ProtoList` of (k,v,k,v,...) alternating, so two keys with a hash
// collision both land in the same bucket and a linear scan resolves
// them. Most buckets hold exactly one (k,v) pair.
//
// Cost model: assoc / get / contains? = O(log N) for the SparseList
// AVL walk + O(B) for the bucket where B is the collision count
// (typically 1). keys / vals = O(N) iteration.
//
// Key equality uses `compare(ctx, other) == 0`, which handles
// SmallInt / LargeInt / Float / Symbol / String identity AND value
// equivalence per the kernel.
//
// Hash fast-path TODO (session 14): for interned symbols / keywords
// the canonical pattern across protoCore (see ProtoObject::getAttribute,
// THREAD_CACHE_DEPTH index) is `(reinterpret_cast<uintptr_t>(key) >> 6)`
// directly — the symbol-table guarantees pointer-stability, and the
// 64-byte cell alignment makes the low 6 bits zero. That avoids the
// virtual `getHash` call entirely for the dominant case. Today we use
// `getHash(ctx)` which is correct for every value type but slower for
// symbols. Switch when the bench says it matters.

const proto::ProtoObject* prim_map_p(proto::ProtoContext* ctx,
                                     const proto::ProtoObject*,
                                     const proto::ParentLink*,
                                     const proto::ProtoList* args,
                                     const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 1)
        throw std::runtime_error("map?: expects 1 arg");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throw std::runtime_error("map?: no active VM context");
    if (!v) return PROTO_FALSE;
    return (v->getPrototype(ctx) == cc->mapMarkerProto) ? PROTO_TRUE : PROTO_FALSE;
}

// Returns the underlying ProtoSparseList of the map wrapper. The wrapper
// stores it as a `__entries__` attribute pointing at a ProtoSparseList's
// asObject form; we round-trip via newSparseList() when the map is fresh.
static const proto::ProtoSparseList* mapEntriesSparse(proto::ProtoContext* ctx,
                                                      const proto::ProtoObject* m,
                                                      const proto::ProtoString* entriesKey) {
    const proto::ProtoObject* raw = m->getAttribute(ctx, entriesKey);
    if (!raw || raw == PROTO_NONE) return ctx->newSparseList();
    return reinterpret_cast<const proto::ProtoSparseList*>(raw);
}

// Inside a hash bucket (a flat ProtoList of k,v,k,v,...), scan for `key`.
// Returns the index of the K cell (always even) for which compare ==
// 0, or -1 if not found.
static long long bucketFindKey(proto::ProtoContext* ctx,
                               const proto::ProtoList* bucket,
                               const proto::ProtoObject* key) {
    if (!bucket) return -1;
    unsigned long n = bucket->getSize(ctx);
    for (unsigned long i = 0; i < n; i += 2) {
        const proto::ProtoObject* k = bucket->getAt(ctx, (int)i);
        if (k->compare(ctx, key) == 0) return (long long)i;
    }
    return -1;
}

static const proto::ProtoObject* buildMap(proto::ProtoContext* ctx,
                                          const ActiveCallContext* cc,
                                          const proto::ProtoSparseList* sparse) {
    proto::ProtoObject* wrap = const_cast<proto::ProtoObject*>(
        cc->mapMarkerProto->newChild(ctx, /*isMutable=*/true));
    wrap->setAttribute(ctx, cc->entriesKey, sparse->asObject(ctx));
    return wrap;
}

// Insert / update (k,v) into a sparse list and return the new sparse.
// Buckets are flat (k,v,k,v,...) ProtoLists keyed by k->getHash(ctx).
static const proto::ProtoSparseList* sparseAssoc(proto::ProtoContext* ctx,
                                                 const proto::ProtoSparseList* sparse,
                                                 const proto::ProtoObject* k,
                                                 const proto::ProtoObject* v) {
    unsigned long h = k->getHash(ctx);
    const proto::ProtoList* bucket = nullptr;
    if (sparse->has(ctx, h)) {
        const proto::ProtoObject* b = sparse->getAt(ctx, h);
        if (b) bucket = b->asList(ctx);
    }
    if (!bucket) bucket = ctx->newList();
    long long idx = bucketFindKey(ctx, bucket, k);
    const proto::ProtoList* newBucket = nullptr;
    if (idx >= 0) {
        newBucket = bucket->setAt(ctx, (int)(idx + 1), v);
    } else {
        newBucket = bucket->appendLast(ctx, k)->appendLast(ctx, v);
    }
    return sparse->setAt(ctx, h, newBucket->asObject(ctx));
}

const proto::ProtoObject* prim_hash_map(proto::ProtoContext* ctx,
                                        const proto::ProtoObject*,
                                        const proto::ParentLink*,
                                        const proto::ProtoList* args,
                                        const proto::ProtoSparseList*) {
    unsigned long n = args ? args->getSize(ctx) : 0;
    if (n % 2 != 0)
        throw std::runtime_error("hash-map: needs an even number of args");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throw std::runtime_error("hash-map: no active VM context");
    const proto::ProtoSparseList* sparse = ctx->newSparseList();
    for (unsigned long i = 0; i < n; i += 2) {
        sparse = sparseAssoc(ctx, sparse,
                             args->getAt(ctx, (int)i),
                             args->getAt(ctx, (int)(i + 1)));
    }
    return buildMap(ctx, cc, sparse);
}

const proto::ProtoObject* prim_assoc(proto::ProtoContext* ctx,
                                     const proto::ProtoObject*,
                                     const proto::ParentLink*,
                                     const proto::ProtoList* args,
                                     const proto::ProtoSparseList*) {
    unsigned long n = args ? args->getSize(ctx) : 0;
    if (n < 3 || (n - 1) % 2 != 0)
        throw std::runtime_error("assoc: expects (assoc m k v ...)");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throw std::runtime_error("assoc: no active VM context");
    const proto::ProtoObject* m = args->getAt(ctx, 0);
    if (!m || m == PROTO_NONE || m->getPrototype(ctx) != cc->mapMarkerProto)
        throw std::runtime_error("assoc: first arg must be a map");
    const proto::ProtoSparseList* sparse =
        mapEntriesSparse(ctx, m, cc->entriesKey);
    for (unsigned long i = 1; i < n; i += 2) {
        sparse = sparseAssoc(ctx, sparse,
                             args->getAt(ctx, (int)i),
                             args->getAt(ctx, (int)(i + 1)));
    }
    return buildMap(ctx, cc, sparse);
}

const proto::ProtoObject* prim_get(proto::ProtoContext* ctx,
                                   const proto::ProtoObject*,
                                   const proto::ParentLink*,
                                   const proto::ProtoList* args,
                                   const proto::ProtoSparseList*) {
    unsigned long n = args ? args->getSize(ctx) : 0;
    if (n != 2 && n != 3)
        throw std::runtime_error("get: expects (get m k) or (get m k not-found)");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throw std::runtime_error("get: no active VM context");
    const proto::ProtoObject* m = args->getAt(ctx, 0);
    const proto::ProtoObject* k = args->getAt(ctx, 1);
    const proto::ProtoObject* nf = (n == 3) ? args->getAt(ctx, 2) : PROTO_NONE;
    if (!m || m == PROTO_NONE) return nf;
    if (m->getPrototype(ctx) != cc->mapMarkerProto) return nf;
    const proto::ProtoSparseList* sparse =
        mapEntriesSparse(ctx, m, cc->entriesKey);
    unsigned long h = k->getHash(ctx);
    if (!sparse->has(ctx, h)) return nf;
    const proto::ProtoObject* b = sparse->getAt(ctx, h);
    if (!b) return nf;
    const proto::ProtoList* bucket = b->asList(ctx);
    long long idx = bucketFindKey(ctx, bucket, k);
    if (idx < 0) return nf;
    return bucket->getAt(ctx, (int)(idx + 1));
}

const proto::ProtoObject* prim_contains_p(proto::ProtoContext* ctx,
                                          const proto::ProtoObject*,
                                          const proto::ParentLink*,
                                          const proto::ProtoList* args,
                                          const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 2)
        throw std::runtime_error("contains?: expects (contains? coll k)");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throw std::runtime_error("contains?: no active VM context");
    const proto::ProtoObject* m = args->getAt(ctx, 0);
    const proto::ProtoObject* k = args->getAt(ctx, 1);
    if (!m || m == PROTO_NONE) return PROTO_FALSE;
    if (m->getPrototype(ctx) != cc->mapMarkerProto) return PROTO_FALSE;
    const proto::ProtoSparseList* sparse =
        mapEntriesSparse(ctx, m, cc->entriesKey);
    unsigned long h = k->getHash(ctx);
    if (!sparse->has(ctx, h)) return PROTO_FALSE;
    const proto::ProtoObject* b = sparse->getAt(ctx, h);
    if (!b) return PROTO_FALSE;
    return bucketFindKey(ctx, b->asList(ctx), k) >= 0 ? PROTO_TRUE : PROTO_FALSE;
}

// Walk every (k,v) in every bucket of the sparse list; return a fresh
// ProtoList of either keys or values depending on `wantValues`. Order
// is sparse-list iteration order (sorted by hash key); within a bucket
// it is insertion order.
static const proto::ProtoObject* mapWalk(proto::ProtoContext* ctx,
                                         const ActiveCallContext* cc,
                                         const proto::ProtoObject* m,
                                         bool wantValues) {
    if (!m || m == PROTO_NONE || m->getPrototype(ctx) != cc->mapMarkerProto)
        return ctx->newList()->asObject(ctx);
    const proto::ProtoSparseList* sparse =
        mapEntriesSparse(ctx, m, cc->entriesKey);

    // protoCore's SparseList exposes processElements for ordered walk;
    // each callback receives (key=hash, value=bucket-list-as-object).
    struct Acc {
        proto::ProtoContext* ctx;
        const proto::ProtoObject* out;
        bool wantValues;
    } acc{ctx, ctx->newList()->asObject(ctx), wantValues};

    sparse->processElements(ctx, &acc,
        [](proto::ProtoContext* c, void* self, unsigned long /*hash*/,
           const proto::ProtoObject* bucketObj) {
            auto* a = static_cast<Acc*>(self);
            if (!bucketObj) return;
            const proto::ProtoList* bucket = bucketObj->asList(c);
            unsigned long n = bucket->getSize(c);
            for (unsigned long i = 0; i < n; i += 2) {
                const proto::ProtoObject* x = bucket->getAt(c,
                    (int)(a->wantValues ? i + 1 : i));
                a->out = a->out->asList(c)->appendLast(c, x)->asObject(c);
            }
        });
    return acc.out;
}

const proto::ProtoObject* prim_keys(proto::ProtoContext* ctx,
                                    const proto::ProtoObject*,
                                    const proto::ParentLink*,
                                    const proto::ProtoList* args,
                                    const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 1)
        throw std::runtime_error("keys: expects 1 arg");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throw std::runtime_error("keys: no active VM context");
    return mapWalk(ctx, cc, args->getAt(ctx, 0), /*wantValues=*/false);
}

const proto::ProtoObject* prim_vals(proto::ProtoContext* ctx,
                                    const proto::ProtoObject*,
                                    const proto::ParentLink*,
                                    const proto::ProtoList* args,
                                    const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 1)
        throw std::runtime_error("vals: expects 1 arg");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throw std::runtime_error("vals: no active VM context");
    return mapWalk(ctx, cc, args->getAt(ctx, 0), /*wantValues=*/true);
}

const proto::ProtoObject* prim_first(proto::ProtoContext* ctx,
                                     const proto::ProtoObject*,
                                     const proto::ParentLink*,
                                     const proto::ProtoList* args,
                                     const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 1)
        throw std::runtime_error("first: expects 1 arg");
    const proto::ProtoList* lst = asSeqOrNull(ctx, args->getAt(ctx, 0));
    if (!lst || lst->getSize(ctx) == 0) return PROTO_NONE;
    return lst->getFirst(ctx);
}

const proto::ProtoObject* prim_rest(proto::ProtoContext* ctx,
                                    const proto::ProtoObject*,
                                    const proto::ParentLink*,
                                    const proto::ProtoList* args,
                                    const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 1)
        throw std::runtime_error("rest: expects 1 arg");
    const proto::ProtoList* lst = asSeqOrNull(ctx, args->getAt(ctx, 0));
    if (!lst || lst->getSize(ctx) == 0) {
        return ctx->newList()->asObject(ctx);
    }
    return lst->removeFirst(ctx)->asObject(ctx);
}

const proto::ProtoObject* prim_cons(proto::ProtoContext* ctx,
                                    const proto::ProtoObject*,
                                    const proto::ParentLink*,
                                    const proto::ProtoList* args,
                                    const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 2)
        throw std::runtime_error("cons: expects (cons x coll)");
    const proto::ProtoObject* x    = args->getAt(ctx, 0);
    const proto::ProtoObject* coll = args->getAt(ctx, 1);
    const proto::ProtoList* lst = asSeqOrNull(ctx, coll);
    if (!lst) lst = ctx->newList();
    return lst->appendFirst(ctx, x)->asObject(ctx);
}

// Session 15 helper — true iff v is a plain ProtoString (string-tag or
// symbol-tag): the canonical "stringy" predicate.
static bool isStringLike(const proto::ProtoObject* v) {
    return v && proto::ProtoObject::isStringTagFast(v);
}

static const proto::ProtoString* asProtoString(const proto::ProtoObject* v) {
    return reinterpret_cast<const proto::ProtoString*>(v);
}

const proto::ProtoObject* prim_count(proto::ProtoContext* ctx,
                                     const proto::ProtoObject*,
                                     const proto::ParentLink*,
                                     const proto::ProtoList* args,
                                     const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 1)
        throw std::runtime_error("count: expects 1 arg");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    if (!v || v == PROTO_NONE) return ctx->fromLong(0);
    if (isStringLike(v)) {
        return ctx->fromLong(static_cast<long long>(asProtoString(v)->getSize(ctx)));
    }
    const proto::ProtoList* lst = asSeqOrNull(ctx, v);
    return ctx->fromLong(lst ? static_cast<long long>(lst->getSize(ctx)) : 0);
}

const proto::ProtoObject* prim_empty_p(proto::ProtoContext* ctx,
                                       const proto::ProtoObject*,
                                       const proto::ParentLink*,
                                       const proto::ProtoList* args,
                                       const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 1)
        throw std::runtime_error("empty?: expects 1 arg");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    if (!v || v == PROTO_NONE) return PROTO_TRUE;
    if (isStringLike(v)) {
        return asProtoString(v)->getSize(ctx) == 0 ? PROTO_TRUE : PROTO_FALSE;
    }
    const proto::ProtoList* lst = asSeqOrNull(ctx, v);
    return (!lst || lst->getSize(ctx) == 0) ? PROTO_TRUE : PROTO_FALSE;
}

const proto::ProtoObject* prim_nil_p(proto::ProtoContext* ctx,
                                     const proto::ProtoObject*,
                                     const proto::ParentLink*,
                                     const proto::ProtoList* args,
                                     const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 1)
        throw std::runtime_error("nil?: expects 1 arg");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    return (!v || v == PROTO_NONE) ? PROTO_TRUE : PROTO_FALSE;
}

const proto::ProtoObject* prim_not(proto::ProtoContext* ctx,
                                   const proto::ProtoObject*,
                                   const proto::ParentLink*,
                                   const proto::ProtoList* args,
                                   const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 1)
        throw std::runtime_error("not: expects 1 arg");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    return (!v || v == PROTO_NONE || v == PROTO_FALSE) ? PROTO_TRUE : PROTO_FALSE;
}

const proto::ProtoObject* prim_reverse(proto::ProtoContext* ctx,
                                       const proto::ProtoObject*,
                                       const proto::ParentLink*,
                                       const proto::ProtoList* args,
                                       const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 1)
        throw std::runtime_error("reverse: expects 1 arg");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    // String input → reverse character by character (codepoint-aware
    // via std::string + UTF-8 walk would be a follow-up; v0.15 reverses
    // raw bytes which is correct for ASCII).
    if (isStringLike(v)) {
        const proto::ProtoString* s = asProtoString(v);
        std::string raw = s->toStdString(ctx);
        std::string r(raw.rbegin(), raw.rend());
        return reinterpret_cast<const proto::ProtoObject*>(
            proto::ProtoString::fromStdString(ctx, r));
    }
    const proto::ProtoList* lst = asSeqOrNull(ctx, v);
    const proto::ProtoObject* out = ctx->newList()->asObject(ctx);
    if (!lst) return out;
    unsigned long n = lst->getSize(ctx);
    for (unsigned long i = 0; i < n; ++i) {
        out = out->asList(ctx)
            ->appendFirst(ctx, lst->getAt(ctx, static_cast<int>(i)))
            ->asObject(ctx);
    }
    return out;
}

// Higher-order primitives (session 7) --------------------------------------

const proto::ProtoObject* prim_map(proto::ProtoContext* ctx,
                                   const proto::ProtoObject*,
                                   const proto::ParentLink*,
                                   const proto::ProtoList* args,
                                   const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 2)
        throw std::runtime_error("map: v0.7.x expects (map f coll)");
    const proto::ProtoObject* f    = args->getAt(ctx, 0);
    const proto::ProtoObject* coll = args->getAt(ctx, 1);
    const proto::ProtoList* lst = asSeqOrNull(ctx, coll);
    const proto::ProtoObject* out = ctx->newList()->asObject(ctx);
    if (!lst) return out;
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throw std::runtime_error("map: no active VM context");
    unsigned long n = lst->getSize(ctx);
    for (unsigned long i = 0; i < n; ++i) {
        const proto::ProtoObject* arg = lst->getAt(ctx, static_cast<int>(i));
        const proto::ProtoObject* one[1] = { arg };
        const proto::ProtoObject* y = cc->engine->invoke(ctx, f, one, 1);
        out = out->asList(ctx)->appendLast(ctx, y)->asObject(ctx);
    }
    return out;
}

const proto::ProtoObject* prim_filter(proto::ProtoContext* ctx,
                                      const proto::ProtoObject*,
                                      const proto::ParentLink*,
                                      const proto::ProtoList* args,
                                      const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 2)
        throw std::runtime_error("filter: expects (filter pred coll)");
    const proto::ProtoObject* pred = args->getAt(ctx, 0);
    const proto::ProtoObject* coll = args->getAt(ctx, 1);
    const proto::ProtoList* lst = asSeqOrNull(ctx, coll);
    const proto::ProtoObject* out = ctx->newList()->asObject(ctx);
    if (!lst) return out;
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throw std::runtime_error("filter: no active VM context");
    unsigned long n = lst->getSize(ctx);
    for (unsigned long i = 0; i < n; ++i) {
        const proto::ProtoObject* x = lst->getAt(ctx, static_cast<int>(i));
        const proto::ProtoObject* one[1] = { x };
        const proto::ProtoObject* keep = cc->engine->invoke(ctx, pred, one, 1);
        if (keep && keep != PROTO_NONE && keep != PROTO_FALSE) {
            out = out->asList(ctx)->appendLast(ctx, x)->asObject(ctx);
        }
    }
    return out;
}

const proto::ProtoObject* prim_reduce(proto::ProtoContext* ctx,
                                      const proto::ProtoObject*,
                                      const proto::ParentLink*,
                                      const proto::ProtoList* args,
                                      const proto::ProtoSparseList*) {
    // Two shapes: (reduce f coll) — init is first elem;
    //             (reduce f init coll) — explicit init.
    unsigned long ac = args ? args->getSize(ctx) : 0;
    if (ac != 2 && ac != 3)
        throw std::runtime_error("reduce: expects (reduce f coll) or (reduce f init coll)");
    const proto::ProtoObject* f = args->getAt(ctx, 0);
    const proto::ProtoObject* coll = args->getAt(ctx, ac == 2 ? 1 : 2);
    const proto::ProtoList* lst = asSeqOrNull(ctx, coll);
    unsigned long n = lst ? lst->getSize(ctx) : 0;
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throw std::runtime_error("reduce: no active VM context");

    const proto::ProtoObject* acc;
    unsigned long start;
    if (ac == 3) {
        acc = args->getAt(ctx, 1);
        start = 0;
    } else {
        if (n == 0) {
            // (reduce f []) → (f) per Clojure
            return cc->engine->invoke(ctx, f, nullptr, 0);
        }
        acc = lst->getFirst(ctx);
        start = 1;
    }
    for (unsigned long i = start; i < n; ++i) {
        const proto::ProtoObject* x = lst->getAt(ctx, static_cast<int>(i));
        const proto::ProtoObject* two[2] = { acc, x };
        acc = cc->engine->invoke(ctx, f, two, 2);
    }
    return acc;
}

// Session 15 — string primitives. `clojure.string`-shaped surface,
// exposed in the global namespace for v0.x (no `ns` yet). UTF-8
// underlies every operation but most ops are ASCII-correct only:
// `upper-case`/`lower-case` go through std::toupper/tolower, search
// ops use byte-level matching. UTF-8 codepoint-correctness is a
// v0.2 follow-up (logged as a deviation in STATUS).

const proto::ProtoObject* prim_subs(proto::ProtoContext* ctx,
                                    const proto::ProtoObject*,
                                    const proto::ParentLink*,
                                    const proto::ProtoList* args,
                                    const proto::ProtoSparseList*) {
    unsigned long ac = args ? args->getSize(ctx) : 0;
    if (ac != 2 && ac != 3)
        throw std::runtime_error("subs: expects (subs s start) or (subs s start end)");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    if (!isStringLike(v))
        throw std::runtime_error("subs: first arg must be a string");
    const proto::ProtoString* s = asProtoString(v);
    long long start = argAsLong(ctx, args, 1, "subs");
    long long sz = static_cast<long long>(s->getSize(ctx));
    long long end = (ac == 3) ? argAsLong(ctx, args, 2, "subs") : sz;
    if (start < 0 || start > sz || end < start || end > sz)
        throw std::runtime_error("subs: bounds out of range");
    return reinterpret_cast<const proto::ProtoObject*>(
        s->getSlice(ctx, static_cast<int>(start), static_cast<int>(end)));
}

const proto::ProtoObject* prim_upper_case(proto::ProtoContext* ctx,
                                          const proto::ProtoObject*,
                                          const proto::ParentLink*,
                                          const proto::ProtoList* args,
                                          const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 1)
        throw std::runtime_error("upper-case: expects 1 arg");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    if (!isStringLike(v))
        throw std::runtime_error("upper-case: arg must be a string");
    std::string raw = asProtoString(v)->toStdString(ctx);
    for (auto& c : raw) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return reinterpret_cast<const proto::ProtoObject*>(
        proto::ProtoString::fromStdString(ctx, raw));
}

const proto::ProtoObject* prim_lower_case(proto::ProtoContext* ctx,
                                          const proto::ProtoObject*,
                                          const proto::ParentLink*,
                                          const proto::ProtoList* args,
                                          const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 1)
        throw std::runtime_error("lower-case: expects 1 arg");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    if (!isStringLike(v))
        throw std::runtime_error("lower-case: arg must be a string");
    std::string raw = asProtoString(v)->toStdString(ctx);
    for (auto& c : raw) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return reinterpret_cast<const proto::ProtoObject*>(
        proto::ProtoString::fromStdString(ctx, raw));
}

const proto::ProtoObject* prim_starts_with_p(proto::ProtoContext* ctx,
                                             const proto::ProtoObject*,
                                             const proto::ParentLink*,
                                             const proto::ProtoList* args,
                                             const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 2)
        throw std::runtime_error("starts-with?: expects (starts-with? s prefix)");
    const proto::ProtoObject* sv = args->getAt(ctx, 0);
    const proto::ProtoObject* pv = args->getAt(ctx, 1);
    if (!isStringLike(sv) || !isStringLike(pv))
        throw std::runtime_error("starts-with?: both args must be strings");
    std::string s = asProtoString(sv)->toStdString(ctx);
    std::string p = asProtoString(pv)->toStdString(ctx);
    return (s.rfind(p, 0) == 0) ? PROTO_TRUE : PROTO_FALSE;
}

const proto::ProtoObject* prim_ends_with_p(proto::ProtoContext* ctx,
                                           const proto::ProtoObject*,
                                           const proto::ParentLink*,
                                           const proto::ProtoList* args,
                                           const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 2)
        throw std::runtime_error("ends-with?: expects (ends-with? s suffix)");
    const proto::ProtoObject* sv = args->getAt(ctx, 0);
    const proto::ProtoObject* pv = args->getAt(ctx, 1);
    if (!isStringLike(sv) || !isStringLike(pv))
        throw std::runtime_error("ends-with?: both args must be strings");
    std::string s = asProtoString(sv)->toStdString(ctx);
    std::string p = asProtoString(pv)->toStdString(ctx);
    if (p.size() > s.size()) return PROTO_FALSE;
    return std::equal(p.rbegin(), p.rend(), s.rbegin()) ? PROTO_TRUE : PROTO_FALSE;
}

const proto::ProtoObject* prim_includes_p(proto::ProtoContext* ctx,
                                          const proto::ProtoObject*,
                                          const proto::ParentLink*,
                                          const proto::ProtoList* args,
                                          const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 2)
        throw std::runtime_error("includes?: expects (includes? s sub)");
    const proto::ProtoObject* sv = args->getAt(ctx, 0);
    const proto::ProtoObject* pv = args->getAt(ctx, 1);
    if (!isStringLike(sv) || !isStringLike(pv))
        throw std::runtime_error("includes?: both args must be strings");
    std::string s = asProtoString(sv)->toStdString(ctx);
    std::string p = asProtoString(pv)->toStdString(ctx);
    return (s.find(p) != std::string::npos) ? PROTO_TRUE : PROTO_FALSE;
}

const proto::ProtoObject* prim_index_of(proto::ProtoContext* ctx,
                                        const proto::ProtoObject*,
                                        const proto::ParentLink*,
                                        const proto::ProtoList* args,
                                        const proto::ProtoSparseList*) {
    unsigned long ac = args ? args->getSize(ctx) : 0;
    if (ac != 2 && ac != 3)
        throw std::runtime_error("index-of: expects (index-of s sub) or (index-of s sub from)");
    const proto::ProtoObject* sv = args->getAt(ctx, 0);
    const proto::ProtoObject* pv = args->getAt(ctx, 1);
    if (!isStringLike(sv) || !isStringLike(pv))
        throw std::runtime_error("index-of: first two args must be strings");
    std::string s = asProtoString(sv)->toStdString(ctx);
    std::string p = asProtoString(pv)->toStdString(ctx);
    std::size_t from = (ac == 3) ? static_cast<std::size_t>(argAsLong(ctx, args, 2, "index-of"))
                                 : 0;
    std::size_t found = s.find(p, from);
    if (found == std::string::npos) return PROTO_NONE;
    return ctx->fromLong(static_cast<long long>(found));
}

const proto::ProtoObject* prim_replace(proto::ProtoContext* ctx,
                                       const proto::ProtoObject*,
                                       const proto::ParentLink*,
                                       const proto::ProtoList* args,
                                       const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 3)
        throw std::runtime_error("replace: expects (replace s match replacement)");
    const proto::ProtoObject* sv = args->getAt(ctx, 0);
    const proto::ProtoObject* mv = args->getAt(ctx, 1);
    const proto::ProtoObject* rv = args->getAt(ctx, 2);
    if (!isStringLike(sv) || !isStringLike(mv) || !isStringLike(rv))
        throw std::runtime_error("replace: all three args must be strings");
    std::string s = asProtoString(sv)->toStdString(ctx);
    std::string m = asProtoString(mv)->toStdString(ctx);
    std::string r = asProtoString(rv)->toStdString(ctx);
    if (m.empty()) return sv;
    std::string out;
    out.reserve(s.size());
    std::size_t pos = 0;
    while (pos < s.size()) {
        std::size_t found = s.find(m, pos);
        if (found == std::string::npos) {
            out.append(s, pos, std::string::npos);
            break;
        }
        out.append(s, pos, found - pos);
        out.append(r);
        pos = found + m.size();
    }
    return reinterpret_cast<const proto::ProtoObject*>(
        proto::ProtoString::fromStdString(ctx, out));
}

// (join sep coll) — concatenate `coll` items, separator between each.
// (join coll) — same with empty separator.
const proto::ProtoObject* prim_join(proto::ProtoContext* ctx,
                                    const proto::ProtoObject*,
                                    const proto::ParentLink*,
                                    const proto::ProtoList* args,
                                    const proto::ProtoSparseList*) {
    unsigned long ac = args ? args->getSize(ctx) : 0;
    if (ac != 1 && ac != 2)
        throw std::runtime_error("join: expects (join coll) or (join sep coll)");
    std::string sep;
    const proto::ProtoObject* coll;
    if (ac == 2) {
        const proto::ProtoObject* sv = args->getAt(ctx, 0);
        if (!isStringLike(sv))
            throw std::runtime_error("join: separator must be a string");
        sep = asProtoString(sv)->toStdString(ctx);
        coll = args->getAt(ctx, 1);
    } else {
        coll = args->getAt(ctx, 0);
    }
    const proto::ProtoList* lst = asSeqOrNull(ctx, coll);
    std::ostringstream os;
    if (lst) {
        unsigned long n = lst->getSize(ctx);
        for (unsigned long i = 0; i < n; ++i) {
            if (i > 0) os << sep;
            appendValue(ctx, os, lst->getAt(ctx, static_cast<int>(i)));
        }
    }
    return reinterpret_cast<const proto::ProtoObject*>(
        proto::ProtoString::fromStdString(ctx, os.str()));
}

// (split s delim) — returns a list of substrings split by `delim`.
const proto::ProtoObject* prim_split(proto::ProtoContext* ctx,
                                     const proto::ProtoObject*,
                                     const proto::ParentLink*,
                                     const proto::ProtoList* args,
                                     const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 2)
        throw std::runtime_error("split: expects (split s delim)");
    const proto::ProtoObject* sv = args->getAt(ctx, 0);
    const proto::ProtoObject* dv = args->getAt(ctx, 1);
    if (!isStringLike(sv) || !isStringLike(dv))
        throw std::runtime_error("split: both args must be strings");
    std::string s = asProtoString(sv)->toStdString(ctx);
    std::string d = asProtoString(dv)->toStdString(ctx);
    const proto::ProtoObject* out = ctx->newList()->asObject(ctx);
    if (d.empty()) {
        // Split into individual codepoints would be ideal; v0.15
        // splits per byte.
        for (char c : s) {
            std::string one(1, c);
            const proto::ProtoString* piece =
                proto::ProtoString::fromStdString(ctx, one);
            out = out->asList(ctx)->appendLast(ctx,
                reinterpret_cast<const proto::ProtoObject*>(piece))->asObject(ctx);
        }
        return out;
    }
    std::size_t pos = 0;
    while (true) {
        std::size_t found = s.find(d, pos);
        std::string piece = (found == std::string::npos)
            ? s.substr(pos)
            : s.substr(pos, found - pos);
        const proto::ProtoString* p =
            proto::ProtoString::fromStdString(ctx, piece);
        out = out->asList(ctx)->appendLast(ctx,
            reinterpret_cast<const proto::ProtoObject*>(p))->asObject(ctx);
        if (found == std::string::npos) break;
        pos = found + d.size();
    }
    return out;
}

static bool isAsciiWs(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

const proto::ProtoObject* prim_trim(proto::ProtoContext* ctx,
                                    const proto::ProtoObject*,
                                    const proto::ParentLink*,
                                    const proto::ProtoList* args,
                                    const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 1)
        throw std::runtime_error("trim: expects 1 arg");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    if (!isStringLike(v))
        throw std::runtime_error("trim: arg must be a string");
    std::string s = asProtoString(v)->toStdString(ctx);
    std::size_t a = 0, b = s.size();
    while (a < b && isAsciiWs(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && isAsciiWs(static_cast<unsigned char>(s[b - 1]))) --b;
    return reinterpret_cast<const proto::ProtoObject*>(
        proto::ProtoString::fromStdString(ctx, s.substr(a, b - a)));
}

const proto::ProtoObject* prim_triml(proto::ProtoContext* ctx,
                                     const proto::ProtoObject*,
                                     const proto::ParentLink*,
                                     const proto::ProtoList* args,
                                     const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 1)
        throw std::runtime_error("triml: expects 1 arg");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    if (!isStringLike(v))
        throw std::runtime_error("triml: arg must be a string");
    std::string s = asProtoString(v)->toStdString(ctx);
    std::size_t a = 0;
    while (a < s.size() && isAsciiWs(static_cast<unsigned char>(s[a]))) ++a;
    return reinterpret_cast<const proto::ProtoObject*>(
        proto::ProtoString::fromStdString(ctx, s.substr(a)));
}

const proto::ProtoObject* prim_trimr(proto::ProtoContext* ctx,
                                     const proto::ProtoObject*,
                                     const proto::ParentLink*,
                                     const proto::ProtoList* args,
                                     const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 1)
        throw std::runtime_error("trimr: expects 1 arg");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    if (!isStringLike(v))
        throw std::runtime_error("trimr: arg must be a string");
    std::string s = asProtoString(v)->toStdString(ctx);
    std::size_t b = s.size();
    while (b > 0 && isAsciiWs(static_cast<unsigned char>(s[b - 1]))) --b;
    return reinterpret_cast<const proto::ProtoObject*>(
        proto::ProtoString::fromStdString(ctx, s.substr(0, b)));
}

const proto::ProtoObject* prim_blank_p(proto::ProtoContext* ctx,
                                       const proto::ProtoObject*,
                                       const proto::ParentLink*,
                                       const proto::ProtoList* args,
                                       const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 1)
        throw std::runtime_error("blank?: expects 1 arg");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    if (!v || v == PROTO_NONE) return PROTO_TRUE;
    if (!isStringLike(v)) return PROTO_FALSE;
    std::string s = asProtoString(v)->toStdString(ctx);
    for (char c : s) {
        if (!isAsciiWs(static_cast<unsigned char>(c))) return PROTO_FALSE;
    }
    return PROTO_TRUE;
}

const proto::ProtoObject* prim_string_p(proto::ProtoContext* ctx,
                                        const proto::ProtoObject*,
                                        const proto::ParentLink*,
                                        const proto::ProtoList* args,
                                        const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 1)
        throw std::runtime_error("string?: expects 1 arg");
    return isStringLike(args->getAt(ctx, 0)) ? PROTO_TRUE : PROTO_FALSE;
}

} // namespace

void installPrimitives(proto::ProtoContext* ctx,
                       proto::ProtoObject* globals) {
    // Install each primitive: wrap the C function pointer in a callable
    // ProtoObject via fromMethod, store on the globals under the symbol key.
    // setAttribute on a mutable receiver mutates in place.
    auto install = [&](const char* name, proto::ProtoMethod fn) {
        const proto::ProtoString* key =
            proto::ProtoString::createSymbol(ctx, name);
        const proto::ProtoObject* callable =
            ctx->fromMethod(nullptr /* self */, fn);
        globals->setAttribute(ctx, key, callable);
    };

    install("println", &prim_println);
    install("+",       &prim_plus);
    install("-",       &prim_minus);
    install("*",       &prim_mul);
    install("inc",     &prim_inc);
    install("dec",     &prim_dec);
    install("<",       &prim_lt);
    install("<=",      &prim_le);
    install(">",       &prim_gt);
    install(">=",      &prim_ge);
    install("=",       &prim_eq);
    install("str",     &prim_str);
    install("/",       &prim_div);

    // Session 7 — collection + higher-order.
    install("list",    &prim_list);
    install("vector",  &prim_vector);
    install("vec",     &prim_vec);
    install("nth",     &prim_nth);
    install("vector?", &prim_vector_p);
    install("list?",   &prim_list_p);

    // Session 13 — maps.
    install("hash-map",  &prim_hash_map);
    install("assoc",     &prim_assoc);
    install("get",       &prim_get);
    install("contains?", &prim_contains_p);
    install("keys",      &prim_keys);
    install("vals",      &prim_vals);
    install("map?",      &prim_map_p);

    // Session 15 — string ops (clojure.string-shaped, in the global
    // namespace since `ns` does not exist yet).
    install("string?",     &prim_string_p);
    install("subs",        &prim_subs);
    install("upper-case",  &prim_upper_case);
    install("lower-case",  &prim_lower_case);
    install("starts-with?",&prim_starts_with_p);
    install("ends-with?",  &prim_ends_with_p);
    install("includes?",   &prim_includes_p);
    install("index-of",    &prim_index_of);
    install("replace",     &prim_replace);
    install("join",        &prim_join);
    install("split",       &prim_split);
    install("trim",        &prim_trim);
    install("triml",       &prim_triml);
    install("trimr",       &prim_trimr);
    install("blank?",      &prim_blank_p);
    install("first",   &prim_first);
    install("rest",    &prim_rest);
    install("cons",    &prim_cons);
    install("count",   &prim_count);
    install("empty?",  &prim_empty_p);
    install("nil?",    &prim_nil_p);
    install("not",     &prim_not);
    install("reverse", &prim_reverse);
    install("map",     &prim_map);
    install("filter",  &prim_filter);
    install("reduce",  &prim_reduce);
}

} // namespace protoClojure
