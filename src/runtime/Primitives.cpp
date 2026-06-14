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

// Pull an argument and coerce to long long, throwing on type mismatch. v0.0.x
// is integer-only for arithmetic; Float/Ratio arrive in session 5+.
long long argAsLong(proto::ProtoContext* ctx, const proto::ProtoList* args,
                    int i, const char* primName) {
    const proto::ProtoObject* a = args->getAt(ctx, i);
    if (!a || !a->isInteger(ctx)) {
        throw std::runtime_error(
            std::string(primName) + ": argument " + std::to_string(i) +
            " is not an integer (v0.0.x is integer-only)");
    }
    return a->asLong(ctx);
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

const proto::ProtoObject* prim_plus(proto::ProtoContext* ctx,
                                    const proto::ProtoObject*,
                                    const proto::ParentLink*,
                                    const proto::ProtoList* args,
                                    const proto::ProtoSparseList*) {
    unsigned long n = args ? args->getSize(ctx) : 0;
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
    long long acc = 1;
    for (unsigned long i = 0; i < n; ++i) acc *= argAsLong(ctx, args, (int)i, "*");
    return ctx->fromLong(acc);
}

const proto::ProtoObject* prim_inc(proto::ProtoContext* ctx,
                                   const proto::ProtoObject*,
                                   const proto::ParentLink*,
                                   const proto::ProtoList* args,
                                   const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 1)
        throw std::runtime_error("inc: expects 1 arg");
    return ctx->fromLong(argAsLong(ctx, args, 0, "inc") + 1);
}

const proto::ProtoObject* prim_dec(proto::ProtoContext* ctx,
                                   const proto::ProtoObject*,
                                   const proto::ParentLink*,
                                   const proto::ProtoList* args,
                                   const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 1)
        throw std::runtime_error("dec: expects 1 arg");
    return ctx->fromLong(argAsLong(ctx, args, 0, "dec") - 1);
}

// Comparison ---------------------------------------------------------------

// Variadic monotonic chain helper. Returns true if for every adjacent pair
// (a, b) the predicate(a, b) holds. Matches Clojure's `(< 1 2 3 4)` etc.
template <typename Pred>
const proto::ProtoObject* monotonicChain(proto::ProtoContext* ctx,
                                         const proto::ProtoList* args,
                                         const char* name,
                                         Pred pred) {
    unsigned long n = args ? args->getSize(ctx) : 0;
    if (n < 2) return PROTO_TRUE;            // 0- or 1-arg form is true
    long long prev = argAsLong(ctx, args, 0, name);
    for (unsigned long i = 1; i < n; ++i) {
        long long cur = argAsLong(ctx, args, (int)i, name);
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
    return monotonicChain(ctx, args, "<", [](long long a, long long b){ return a < b; });
}

const proto::ProtoObject* prim_le(proto::ProtoContext* ctx,
                                  const proto::ProtoObject*,
                                  const proto::ParentLink*,
                                  const proto::ProtoList* args,
                                  const proto::ProtoSparseList*) {
    return monotonicChain(ctx, args, "<=", [](long long a, long long b){ return a <= b; });
}

const proto::ProtoObject* prim_gt(proto::ProtoContext* ctx,
                                  const proto::ProtoObject*,
                                  const proto::ParentLink*,
                                  const proto::ProtoList* args,
                                  const proto::ProtoSparseList*) {
    return monotonicChain(ctx, args, ">", [](long long a, long long b){ return a > b; });
}

const proto::ProtoObject* prim_ge(proto::ProtoContext* ctx,
                                  const proto::ProtoObject*,
                                  const proto::ParentLink*,
                                  const proto::ProtoList* args,
                                  const proto::ProtoSparseList*) {
    return monotonicChain(ctx, args, ">=", [](long long a, long long b){ return a >= b; });
}

const proto::ProtoObject* prim_eq(proto::ProtoContext* ctx,
                                  const proto::ProtoObject*,
                                  const proto::ParentLink*,
                                  const proto::ProtoList* args,
                                  const proto::ProtoSparseList*) {
    // = on integers in v0.0.x — for collections / strings we need structural
    // equality which lands later. Variadic chain like Clojure's =.
    return monotonicChain(ctx, args, "=", [](long long a, long long b){ return a == b; });
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

// Treat nil as the empty list for traversal — Clojure's seq nil == nil but
// (first nil) == nil, (rest nil) == (), (count nil) == 0. We collapse the
// distinction in v0.7.x: anything that isn't a list and isn't nil/false
// is an error when given to first/rest/count.
const proto::ProtoList* asSeqOrNull(proto::ProtoContext* ctx,
                                    const proto::ProtoObject* v) {
    if (!v || v == PROTO_NONE) return nullptr;
    if (!isListTag(v)) {
        throw std::runtime_error(
            "seq op: argument is not a list (v0.7.x lists-only)");
    }
    return v->asList(ctx);
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

    // Session 7 — collection + higher-order.
    install("list",    &prim_list);
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
