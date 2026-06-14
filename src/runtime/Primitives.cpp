#include "Primitives.h"

#include "protoCore.h"

#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <string>

namespace protoClojure {

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
}

} // namespace protoClojure
