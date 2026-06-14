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
        std::fprintf(out, "%lld", v->asLong(ctx));
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
    std::fputs("#<unprintable>", out);
}

void appendValue(proto::ProtoContext* ctx, std::ostringstream& os,
                 const proto::ProtoObject* v) {
    if (!v || v == PROTO_NONE) { os << "nil"; return; }
    if (v == PROTO_TRUE)        { os << "true"; return; }
    if (v == PROTO_FALSE)       { os << "false"; return; }
    if (v->isInteger(ctx))      { os << v->asLong(ctx); return; }
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

const proto::ProtoObject* prim_count(proto::ProtoContext* ctx,
                                     const proto::ProtoObject*,
                                     const proto::ParentLink*,
                                     const proto::ProtoList* args,
                                     const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 1)
        throw std::runtime_error("count: expects 1 arg");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    if (!v || v == PROTO_NONE) return ctx->fromLong(0);
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
    const proto::ProtoList* lst = asSeqOrNull(ctx, args->getAt(ctx, 0));
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
