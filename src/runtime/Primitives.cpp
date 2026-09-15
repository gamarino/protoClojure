#include "Primitives.h"
#include "ExecutionEngine.h"
#include "ActorScheduler.h"
#include "MapOps.h"
#include "StackGuard.h"

#include "protoCore.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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

// Argument `i` as a long long, for index and position arguments (nth,
// subs, index-of). An integer beyond the long long range is an error.
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

// Argument `i` of a numeric primitive: an integer (SmallInteger or
// LargeInteger) or a float. Anything else raises the ClassCastException
// analogue naming the primitive and the argument's type (throwNotANumber),
// the same error the arithmetic opcodes raise.
const proto::ProtoObject* numberArg(proto::ProtoContext* ctx,
                                    const proto::ProtoList* args,
                                    int i, const char* primName) {
    const proto::ProtoObject* a = args->getAt(ctx, i);
    if (isNumber(ctx, a)) return a;
    throwNotANumber(ctx, primName, a);
}

// The sign of a - b for two integers, exact at every magnitude: two
// SmallIntegers compare inline (tagged-pointer layout, headers/protoCore.h);
// anything larger goes through protoCore compare.
int compareIntegers(proto::ProtoContext* ctx, const proto::ProtoObject* a,
                    const proto::ProtoObject* b) {
    constexpr unsigned long kSmallIntMask  = 0x3FFUL;
    constexpr unsigned long kSmallIntValue = 0x001UL;
    if ((reinterpret_cast<unsigned long>(a) & kSmallIntMask) == kSmallIntValue &&
        (reinterpret_cast<unsigned long>(b) & kSmallIntMask) == kSmallIntValue) {
        const long long x = reinterpret_cast<long long>(a) >> 10;
        const long long y = reinterpret_cast<long long>(b) >> 10;
        return (x > y) - (x < y);
    }
    return a->compare(ctx, b);
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

// True for a protoCore object cell (POINTER_TAG_OBJECT = 0): maps, keywords
// and symbols, atoms, functions, futures, promises and actors.
bool isObjectTag(const proto::ProtoObject* v) {
    return v && (reinterpret_cast<uintptr_t>(v) & 0x3F) == 0;
}

inline MapLayout mapLayoutOf(const ActiveCallContext* cc) {
    return MapLayout{cc->mapMarkerProto, cc->mapStateKey};
}

// The name a built-in function is installed under, or nullptr when `fn` is
// not one of the primitives in kPrimitives (defined after the primitives).
const char* primitiveName(proto::ProtoMethod fn);

// A string in its readable form: quoted, with the characters Clojure's
// printer escapes (", \, newline, tab, return, form feed, backspace) written
// as escape sequences. Every other byte, UTF-8 included, is copied as is.
void appendReadableString(std::string& out, const std::string& bytes) {
    out += '"';
    for (char c : bytes) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\t': out += "\\t";  break;
            case '\r': out += "\\r";  break;
            case '\f': out += "\\f";  break;
            case '\b': out += "\\b";  break;
            default:   out += c;      break;
        }
    }
    out += '"';
}

// The value printer. `println`, `str`, `join` and the REPL all render values
// through printTo, so a value reads the same wherever it is printed. It
// appends the printed form of `v` to `out`:
//   - nil, booleans and integers as literals; floats as JVM Clojure prints
//     them (formatDouble: `100.0`, `0.3333333333333333`, `1.0E21`, `-0.0`,
//     `##Inf`);
//   - strings as their characters when `readable` is false (Clojure's
//     `print`), or quoted and escaped when it is true (Clojure's `pr`); the
//     mode applies at every depth;
//   - keywords and symbols as their spelling;
//   - lists `(1 2)`, vectors `[1 2]` and maps `{:a 1, :b 2}` (insertion
//     order), recursively;
//   - atoms `#<atom 1>`, futures `#<future 1>` / `#<future pending>`,
//     promises `#<promise 1>` / `#<promise pending>`, actors `#<actor 1>`
//     (the current state, printed in the same mode), user fns `#<fn>` and
//     built-in functions `#<fn NAME>` (`#<fn println>`);
//   - anything else as `#<unprintable>`.
// Runtime objects are recognised by the prototypes in the ActiveCallContext;
// without one only the literal kinds and collections render.
//
// Runtime strings are plain protoCore strings (the Reader's string wrapper
// never reaches the VM), so their bytes are read directly.
//
// GC: allocates only to render a LargeInteger. `v` must be rooted by the
// caller; every nested value is reachable from it. Recurses once per level
// of nesting: a collection nested deeper than the stack allows raises
// StackOverflowError.
void printTo(proto::ProtoContext* ctx, std::string& out,
             const proto::ProtoObject* v, bool readable) {
    checkNativeStack();
    if (!v || v == PROTO_NONE) { out += "nil"; return; }
    if (v == PROTO_TRUE)        { out += "true"; return; }
    if (v == PROTO_FALSE)       { out += "false"; return; }
    if (v->isInteger(ctx)) {
        // SmallInt: the tagged-pointer fast extract, mirror of the check in
        // ExecutionEngine.cpp. LargeInt: protoCore's asIntegerString.
        constexpr unsigned long kSmallIntMask  = 0x3FFUL;
        constexpr unsigned long kSmallIntValue = 0x001UL;
        unsigned long bits = reinterpret_cast<unsigned long>(v);
        if ((bits & kSmallIntMask) == kSmallIntValue) {
            out += std::to_string(reinterpret_cast<long long>(v) >> 10);
        } else {
            out += v->asIntegerString(ctx)->toStdString(ctx);
        }
        return;
    }
    if (v->isFloat(ctx)) {
        out += formatDouble(v->asDouble(ctx));
        return;
    }
    if (isListTag(v)) {
        const proto::ProtoList* lst = v->asList(ctx);
        out += '(';
        unsigned long n = lst->getSize(ctx);
        for (unsigned long i = 0; i < n; ++i) {
            if (i > 0) out += ' ';
            printTo(ctx, out, lst->getAt(ctx, static_cast<int>(i)), readable);
        }
        out += ')';
        return;
    }
    if (v->isTuple(ctx)) {
        const proto::ProtoTuple* t =
            reinterpret_cast<const proto::ProtoTuple*>(v);
        out += '[';
        unsigned long n = t->getSize(ctx);
        for (unsigned long i = 0; i < n; ++i) {
            if (i > 0) out += ' ';
            printTo(ctx, out, t->getAt(ctx, static_cast<int>(i)), readable);
        }
        out += ']';
        return;
    }
    if (proto::ProtoObject::isStringTagFast(v)) {
        const std::string bytes =
            reinterpret_cast<const proto::ProtoString*>(v)->toStdString(ctx);
        if (readable) appendReadableString(out, bytes);
        else          out += bytes;
        return;
    }
    // Built-in functions are protoCore method cells (a pointer-tag check).
    if (v->isMethod(ctx)) {
        const char* name = primitiveName(v->asMethod(ctx));
        out += "#<fn";
        if (name) { out += ' '; out += name; }
        out += '>';
        return;
    }
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) { out += "#<unprintable>"; return; }
    if (isNamed(ctx, cc->named, v)) {
        out += namedSpelling(ctx, cc->named, v)->toStdString(ctx);
        return;
    }
    const proto::ProtoObject* prototype = v->getPrototype(ctx);
    if (prototype == cc->mapMarkerProto) {
        out += '{';
        struct Acc { std::string* out; bool readable; bool first; }
            acc{&out, readable, true};
        mapForEach(ctx, mapLayoutOf(cc), v, &acc,
            [](proto::ProtoContext* c, void* self,
               const proto::ProtoObject* k, const proto::ProtoObject* val) {
                auto* a = static_cast<Acc*>(self);
                if (!a->first) *a->out += ", ";
                a->first = false;
                printTo(c, *a->out, k, a->readable);
                *a->out += ' ';
                printTo(c, *a->out, val, a->readable);
            });
        out += '}';
        return;
    }
    if (prototype == cc->atomMarkerProto) {
        const proto::ProtoObject* inner = v->getAttribute(ctx, cc->valueKey);
        out += "#<atom ";
        printTo(ctx, out, inner ? inner : PROTO_NONE, readable);
        out += '>';
        return;
    }
    if (prototype == cc->futureMarkerProto) {
        if (v->getAttribute(ctx, cc->doneKey) == PROTO_TRUE) {
            out += "#<future ";
            printTo(ctx, out, v->getAttribute(ctx, cc->resultKey), readable);
            out += '>';
        } else {
            out += "#<future pending>";
        }
        return;
    }
    if (prototype == cc->promiseMarkerProto) {
        if (v->getAttribute(ctx, cc->doneKey) == PROTO_TRUE) {
            out += "#<promise ";
            printTo(ctx, out, v->getAttribute(ctx, cc->valueKey), readable);
            out += '>';
        } else {
            out += "#<promise pending>";
        }
        return;
    }
    if (prototype == cc->actorMarkerProto) {
        out += "#<actor ";
        printTo(ctx, out, v->getAttribute(ctx, cc->valueKey), readable);
        out += '>';
        return;
    }
    // User fns, single- and multi-arity. A top-level `(defn sq ...)` returns
    // the fn itself; JVM Clojure prints the var `#'user/sq`, and protoClojure
    // has no vars yet.
    if (prototype == cc->fnSingleProto || prototype == cc->fnMultiProto) {
        out += "#<fn>";
        return;
    }
    out += "#<unprintable>";
}

// printTo, written to `stream` in one call.
void printValue(proto::ProtoContext* ctx, std::FILE* stream,
                const proto::ProtoObject* v, bool readable) {
    std::string text;
    printTo(ctx, text, v, readable);
    std::fwrite(text.data(), 1, text.size(), stream);
}

// Appends `v` as `str` renders one argument: nil as nothing, a string as its
// characters, and any other value in its readable form, so the strings
// nested in a collection keep their quotes (`(str "a" ["b"])` is `a["b"]`).
// `join` renders each element the same way. A bare infinity or NaN is
// spelled `Infinity`, `-Infinity` or `NaN`, as Java's Double.toString gives
// JVM Clojure's `str`; inside a collection it prints as `##Inf`.
void appendStr(proto::ProtoContext* ctx, std::string& out,
               const proto::ProtoObject* v) {
    if (!v || v == PROTO_NONE) return;
    if (proto::ProtoObject::isStringTagFast(v)) {
        out += reinterpret_cast<const proto::ProtoString*>(v)->toStdString(ctx);
        return;
    }
    if (v->isDouble(ctx)) {
        const double d = v->asDouble(ctx);
        if (std::isnan(d)) { out += "NaN"; return; }
        if (std::isinf(d)) { out += d > 0 ? "Infinity" : "-Infinity"; return; }
    }
    printTo(ctx, out, v, /*readable=*/true);
}

// println — print each positional arg, space-separated, followed by \n.
// Returns nil (PROTO_NONE) per Clojure-JVM semantics.
//
// P1: walks the positional-args ProtoList by index, immediately consuming
// each element. No intermediate ProtoObject* held across an allocation
// that we can avoid. The println'd objects are read-only inspected for
// their string representation.
//
// P3: the line is rendered into a std::string on the C++ stack (printTo) and
// written with one fwrite; no std container is stored inside protoCore.
const proto::ProtoObject* prim_println(proto::ProtoContext* ctx,
                                       const proto::ProtoObject* /*self*/,
                                       const proto::ParentLink* /*parents*/,
                                       const proto::ProtoList* args,
                                       const proto::ProtoSparseList* /*kwargs*/) {
    unsigned long n = args ? args->getSize(ctx) : 0;
    std::string line;
    for (unsigned long i = 0; i < n; ++i) {
        if (i > 0) line += ' ';
        printTo(ctx, line, args->getAt(ctx, static_cast<int>(i)),
                /*readable=*/false);
    }
    line += '\n';
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
    return PROTO_NONE;
}

// Arithmetic ---------------------------------------------------------------
//
// Integer arithmetic is exact: protoCore's add, subtract, multiply and
// divide keep a result that fits a SmallInteger inline and promote it to a
// LargeInteger otherwise, so no integer operation wraps around (deviation
// D14); the SmallInteger arithmetic opcodes fall back to the same calls. A
// float operand makes the result a double. Every argument is checked to be a
// number before any arithmetic runs.

// Clojure's variadic left fold, (op (op a b) c ...), over n >= 2 numeric
// arguments. With more than two arguments the running result is a C++ local
// held across allocations, so that loop runs in a GC critical section.
template <typename Op>
const proto::ProtoObject* numericFold(proto::ProtoContext* ctx,
                                      const proto::ProtoList* args,
                                      unsigned long n, Op op) {
    if (n == 2) return op(args->getAt(ctx, 0), args->getAt(ctx, 1));
    proto::ProtoContext::CriticalSection guard(ctx);
    const proto::ProtoObject* acc = args->getAt(ctx, 0);
    for (unsigned long i = 1; i < n; ++i)
        acc = op(acc, args->getAt(ctx, static_cast<int>(i)));
    return acc;
}

void checkNumbers(proto::ProtoContext* ctx, const proto::ProtoList* args,
                  unsigned long n, const char* primName) {
    for (unsigned long i = 0; i < n; ++i)
        numberArg(ctx, args, static_cast<int>(i), primName);
}

const proto::ProtoObject* prim_plus(proto::ProtoContext* ctx,
                                    const proto::ProtoObject*,
                                    const proto::ParentLink*,
                                    const proto::ProtoList* args,
                                    const proto::ProtoSparseList*) {
    unsigned long n = args ? args->getSize(ctx) : 0;
    checkNumbers(ctx, args, n, "+");
    if (n == 0) return ctx->fromLong(0);
    if (n == 1) return args->getAt(ctx, 0);
    return numericFold(ctx, args, n,
        [ctx](const proto::ProtoObject* a, const proto::ProtoObject* b) {
            return a->add(ctx, b);
        });
}

const proto::ProtoObject* prim_minus(proto::ProtoContext* ctx,
                                     const proto::ProtoObject*,
                                     const proto::ParentLink*,
                                     const proto::ProtoList* args,
                                     const proto::ProtoSparseList*) {
    unsigned long n = args ? args->getSize(ctx) : 0;
    if (n == 0) throw std::runtime_error("- : needs at least one arg");
    checkNumbers(ctx, args, n, "-");
    if (n == 1) {
        // Negation. A float keeps its sign bit, so (- 0.0) is -0.0.
        const proto::ProtoObject* x = args->getAt(ctx, 0);
        if (x->isFloat(ctx)) return ctx->fromDouble(-x->asDouble(ctx));
        return ctx->fromLong(0)->subtract(ctx, x);
    }
    return numericFold(ctx, args, n,
        [ctx](const proto::ProtoObject* a, const proto::ProtoObject* b) {
            return a->subtract(ctx, b);
        });
}

const proto::ProtoObject* prim_mul(proto::ProtoContext* ctx,
                                   const proto::ProtoObject*,
                                   const proto::ParentLink*,
                                   const proto::ProtoList* args,
                                   const proto::ProtoSparseList*) {
    unsigned long n = args ? args->getSize(ctx) : 0;
    checkNumbers(ctx, args, n, "*");
    if (n == 0) return ctx->fromLong(1);
    if (n == 1) return args->getAt(ctx, 0);
    return numericFold(ctx, args, n,
        [ctx](const proto::ProtoObject* a, const proto::ProtoObject* b) {
            return a->multiply(ctx, b);
        });
}

// `/` — when every argument is an integer, the quotient truncates toward
// zero (there are no ratios yet) and stays exact at any magnitude; once any
// argument is a float, every argument is taken as a double.
const proto::ProtoObject* prim_div(proto::ProtoContext* ctx,
                                   const proto::ProtoObject*,
                                   const proto::ParentLink*,
                                   const proto::ProtoList* args,
                                   const proto::ProtoSparseList*) {
    unsigned long n = args ? args->getSize(ctx) : 0;
    if (n == 0) throw std::runtime_error("/: needs at least one arg");
    checkNumbers(ctx, args, n, "/");
    bool anyFloat = false;
    for (unsigned long i = 0; i < n && !anyFloat; ++i)
        anyFloat = args->getAt(ctx, static_cast<int>(i))->isFloat(ctx);
    if (anyFloat) {
        if (n == 1) return ctx->fromDouble(1.0 / args->getAt(ctx, 0)->asDouble(ctx));
        double acc = args->getAt(ctx, 0)->asDouble(ctx);
        for (unsigned long i = 1; i < n; ++i) {
            double v = args->getAt(ctx, static_cast<int>(i))->asDouble(ctx);
            if (v == 0.0) throw std::runtime_error("/: divide by zero");
            acc /= v;
        }
        return ctx->fromDouble(acc);
    }
    for (unsigned long i = (n == 1 ? 0 : 1); i < n; ++i) {
        if (args->getAt(ctx, static_cast<int>(i))->integerSign(ctx) == 0)
            throw std::runtime_error("/: divide by zero");
    }
    if (n == 1) return ctx->fromLong(1)->divide(ctx, args->getAt(ctx, 0));
    return numericFold(ctx, args, n,
        [ctx](const proto::ProtoObject* a, const proto::ProtoObject* b) {
            return a->divide(ctx, b);
        });
}

const proto::ProtoObject* prim_inc(proto::ProtoContext* ctx,
                                   const proto::ProtoObject*,
                                   const proto::ParentLink*,
                                   const proto::ProtoList* args,
                                   const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 1)
        throw std::runtime_error("inc: expects 1 arg");
    const proto::ProtoObject* x = numberArg(ctx, args, 0, "inc");
    if (x->isFloat(ctx)) return ctx->fromDouble(x->asDouble(ctx) + 1.0);
    return x->add(ctx, ctx->fromLong(1));
}

const proto::ProtoObject* prim_dec(proto::ProtoContext* ctx,
                                   const proto::ProtoObject*,
                                   const proto::ParentLink*,
                                   const proto::ProtoList* args,
                                   const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 1)
        throw std::runtime_error("dec: expects 1 arg");
    const proto::ProtoObject* x = numberArg(ctx, args, 0, "dec");
    if (x->isFloat(ctx)) return ctx->fromDouble(x->asDouble(ctx) - 1.0);
    return x->subtract(ctx, ctx->fromLong(1));
}

// Comparison ---------------------------------------------------------------

// Variadic monotonic chain: true when the predicate holds for every adjacent
// pair (a, b). Two integers compare exactly (compareIntegers), so integers
// beyond 2^53 are not rounded; a pair involving a float compares as doubles,
// which keeps every comparison with NaN false. Arguments are checked pair by
// pair and the chain stops at the first pair that fails, as in Clojure.
template <typename DoublePred, typename SignPred>
const proto::ProtoObject* monotonicChain(proto::ProtoContext* ctx,
                                         const proto::ProtoList* args,
                                         const char* name,
                                         DoublePred holdsForDoubles,
                                         SignPred holdsForSign) {
    unsigned long n = args ? args->getSize(ctx) : 0;
    if (n < 2) return PROTO_TRUE;            // 0- or 1-arg form is true
    const proto::ProtoObject* prev = numberArg(ctx, args, 0, name);
    for (unsigned long i = 1; i < n; ++i) {
        const proto::ProtoObject* cur = numberArg(ctx, args, (int)i, name);
        const bool holds = (prev->isInteger(ctx) && cur->isInteger(ctx))
            ? holdsForSign(compareIntegers(ctx, prev, cur))
            : holdsForDoubles(prev->asDouble(ctx), cur->asDouble(ctx));
        if (!holds) return PROTO_FALSE;
        prev = cur;
    }
    return PROTO_TRUE;
}

const proto::ProtoObject* prim_lt(proto::ProtoContext* ctx,
                                  const proto::ProtoObject*,
                                  const proto::ParentLink*,
                                  const proto::ProtoList* args,
                                  const proto::ProtoSparseList*) {
    return monotonicChain(ctx, args, "<", [](double a, double b){ return a < b; },
                          [](int sign){ return sign < 0; });
}

const proto::ProtoObject* prim_le(proto::ProtoContext* ctx,
                                  const proto::ProtoObject*,
                                  const proto::ParentLink*,
                                  const proto::ProtoList* args,
                                  const proto::ProtoSparseList*) {
    return monotonicChain(ctx, args, "<=", [](double a, double b){ return a <= b; },
                          [](int sign){ return sign <= 0; });
}

const proto::ProtoObject* prim_gt(proto::ProtoContext* ctx,
                                  const proto::ProtoObject*,
                                  const proto::ParentLink*,
                                  const proto::ProtoList* args,
                                  const proto::ProtoSparseList*) {
    return monotonicChain(ctx, args, ">", [](double a, double b){ return a > b; },
                          [](int sign){ return sign > 0; });
}

const proto::ProtoObject* prim_ge(proto::ProtoContext* ctx,
                                  const proto::ProtoObject*,
                                  const proto::ParentLink*,
                                  const proto::ProtoList* args,
                                  const proto::ProtoSparseList*) {
    return monotonicChain(ctx, args, ">=", [](double a, double b){ return a >= b; },
                          [](int sign){ return sign >= 0; });
}

const proto::ProtoObject* prim_eq(proto::ProtoContext* ctx,
                                  const proto::ProtoObject*,
                                  const proto::ParentLink*,
                                  const proto::ProtoList* args,
                                  const proto::ProtoSparseList*) {
    // Value equality (valuesEqual) over every adjacent pair, like Clojure's
    // variadic =. The 0- and 1-argument forms are true.
    unsigned long n = args ? args->getSize(ctx) : 0;
    if (n < 2) return PROTO_TRUE;
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throw std::runtime_error("=: no active VM context");
    const MapLayout layout = mapLayoutOf(cc);
    for (unsigned long i = 1; i < n; ++i) {
        if (!valuesEqual(ctx, layout, args->getAt(ctx, static_cast<int>(i - 1)),
                         args->getAt(ctx, static_cast<int>(i))))
            return PROTO_FALSE;
    }
    return PROTO_TRUE;
}

const proto::ProtoObject* prim_not_eq(proto::ProtoContext* ctx,
                                      const proto::ProtoObject* self,
                                      const proto::ParentLink* parentLink,
                                      const proto::ProtoList* args,
                                      const proto::ProtoSparseList* kwArgs) {
    return prim_eq(ctx, self, parentLink, args, kwArgs) == PROTO_TRUE
               ? PROTO_FALSE : PROTO_TRUE;
}

// str ----------------------------------------------------------------------

// (str x y z) → every argument rendered by appendStr, concatenated: nil is
// "", a string is inserted as is, and any other value is printed readably,
// so `(str {:a "b"})` is "{:a \"b\"}" and `(str (atom 1))` is "#<atom 1>".
const proto::ProtoObject* prim_str(proto::ProtoContext* ctx,
                                   const proto::ProtoObject*,
                                   const proto::ParentLink*,
                                   const proto::ProtoList* args,
                                   const proto::ProtoSparseList*) {
    unsigned long n = args ? args->getSize(ctx) : 0;
    std::string text;
    for (unsigned long i = 0; i < n; ++i) {
        appendStr(ctx, text, args->getAt(ctx, (int)i));
    }
    return ctx->fromUTF8String(text.c_str());
}

// List + predicate primitives -----------------------------------

// Treat nil as the empty seq. Vectors (ProtoTuple) are seq-coerced via
// ProtoTuple::asList, which converts in O(N) but is fine for v0.9
// benchmarks (consumers walk linearly anyway). Strings stay opaque for
// the first version; later changes added string support.
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

// Session 13 — map primitives. The representation (insertion-ordered
// entries plus a hash index), its cost model and its GC-rooting rules
// live in src/runtime/MapOps.h; the primitives below only validate
// arguments and delegate. Keys are hashed with valueHash and matched with
// valuesEqual, so keys match by value. Map equality under `=` is mapEquals,
// reached through valuesEqual.

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

// Also the target of every `{...}` literal, whose entries the compiler
// passes in source order.
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
    return mapAssocPairs(ctx, mapLayoutOf(cc), nullptr, args, 0, n);
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
    if (!isMap(ctx, mapLayoutOf(cc), m))
        throw std::runtime_error("assoc: first arg must be a map");
    return mapAssocPairs(ctx, mapLayoutOf(cc), m, args, 1, n - 1);
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
    if (!isMap(ctx, mapLayoutOf(cc), m)) return nf;
    bool found = false;
    const proto::ProtoObject* v = mapGet(ctx, mapLayoutOf(cc), m, k, &found);
    return found ? v : nf;
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
    if (!isMap(ctx, mapLayoutOf(cc), m)) return PROTO_FALSE;
    bool found = false;
    mapGet(ctx, mapLayoutOf(cc), m, k, &found);
    return found ? PROTO_TRUE : PROTO_FALSE;
}

// Return a fresh ProtoList of the keys or the values of `m`, in insertion
// order. The list under construction lives in an automatic local (P1).
static const proto::ProtoObject* mapWalk(proto::ProtoContext* ctx,
                                         const ActiveCallContext* cc,
                                         const proto::ProtoObject* m,
                                         bool wantValues) {
    if (!isMap(ctx, mapLayoutOf(cc), m))
        return ctx->newList()->asObject(ctx);
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(1);
    scope.setAutomaticLocal(0, scope.newList()->asObject(&scope));
    struct Acc { proto::ProtoContext* scope; bool wantValues; }
        acc{&scope, wantValues};
    mapForEach(&scope, mapLayoutOf(cc), m, &acc,
        [](proto::ProtoContext* c, void* self,
           const proto::ProtoObject* k, const proto::ProtoObject* v) {
            auto* a = static_cast<Acc*>(self);
            const proto::ProtoList* cur =
                a->scope->getAutomaticLocal(0)->asList(c);
            a->scope->setAutomaticLocal(0,
                cur->appendLast(c, a->wantValues ? v : k)->asObject(c));
        });
    return scope.getAutomaticLocal(0);
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

// Higher-order primitives --------------------------------------

const proto::ProtoObject* prim_map(proto::ProtoContext* ctx,
                                   const proto::ProtoObject*,
                                   const proto::ParentLink*,
                                   const proto::ProtoList* args,
                                   const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 2)
        throw std::runtime_error(
            "map: only (map f coll) is supported; multi-collection map is not implemented yet");
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
    std::string text;
    if (lst) {
        unsigned long n = lst->getSize(ctx);
        for (unsigned long i = 0; i < n; ++i) {
            if (i > 0) text += sep;
            appendStr(ctx, text, lst->getAt(ctx, static_cast<int>(i)));
        }
    }
    return reinterpret_cast<const proto::ProtoObject*>(
        proto::ProtoString::fromStdString(ctx, text));
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

// Session 16 — atoms. The wire shape is a child of atomMarkerProto
// with the current value stored as the `__value__` own-attribute.
// reset! does an in-place setAttribute (protoCore handles the CAS
// loop internally for mutables, so single-writer correctness is
// free). swap! runs a userfn-aware retry loop on top of
// setAttributeIfEqual: read the OWN value, derive new via (f old
// extra-args...), CAS, retry on mismatch. compare-and-set!
// exposes the primitive single-attempt CAS to Clojure code.

const proto::ProtoObject* prim_atom(proto::ProtoContext* ctx,
                                    const proto::ProtoObject*,
                                    const proto::ParentLink*,
                                    const proto::ProtoList* args,
                                    const proto::ProtoSparseList*) {
    unsigned long n = args ? args->getSize(ctx) : 0;
    if (n != 1)
        throw std::runtime_error("atom: expects (atom initial-value)");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throw std::runtime_error("atom: no active VM context");
    proto::ProtoObject* a = const_cast<proto::ProtoObject*>(
        cc->atomMarkerProto->newChild(ctx, /*isMutable=*/true));
    a->setAttribute(ctx, cc->valueKey, args->getAt(ctx, 0));
    return a;
}

const proto::ProtoObject* prim_atom_p(proto::ProtoContext* ctx,
                                      const proto::ProtoObject*,
                                      const proto::ParentLink*,
                                      const proto::ProtoList* args,
                                      const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 1)
        throw std::runtime_error("atom?: expects 1 arg");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throw std::runtime_error("atom?: no active VM context");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    if (!v) return PROTO_FALSE;
    return (v->getPrototype(ctx) == cc->atomMarkerProto) ? PROTO_TRUE : PROTO_FALSE;
}

const proto::ProtoObject* prim_deref(proto::ProtoContext* ctx,
                                     const proto::ProtoObject*,
                                     const proto::ParentLink*,
                                     const proto::ProtoList* args,
                                     const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 1)
        throw std::runtime_error("deref: expects 1 arg");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throw std::runtime_error("deref: no active VM context");
    const proto::ProtoObject* a = args->getAt(ctx, 0);
    if (!a) throw std::runtime_error("deref: nil");
    const proto::ProtoObject* proto = a->getPrototype(ctx);
    if (proto == cc->atomMarkerProto) {
        const proto::ProtoObject* v = a->getAttribute(ctx, cc->valueKey);
        return v ? v : PROTO_NONE;
    }
    if (proto == cc->futureMarkerProto) {
        const proto::ProtoObject* done =
            a->getAttribute(ctx, cc->doneKey);
        if (done != PROTO_TRUE) {
            const proto::ProtoObject* tObj =
                a->getAttribute(ctx, cc->threadKey);
            if (tObj && tObj->isInteger(ctx)) {
                proto::ProtoThread* t =
                    reinterpret_cast<proto::ProtoThread*>(tObj->asLong(ctx));
                if (t) {
                    // Blocking wait: run unmanaged so the future's
                    // thread can complete a GC cycle it requests.
                    proto::ProtoContext::UnmanagedScope unmanaged(ctx);
                    t->join(ctx);
                }
            }
        }
        const proto::ProtoObject* r =
            a->getAttribute(ctx, cc->resultKey);
        return r ? r : PROTO_NONE;
    }
    if (proto == cc->actorMarkerProto) {
        // Read the current state mirror. Same key as atoms, so the
        // ordinary attribute-cache path picks it up after the first
        // hit on a long-lived actor.
        const proto::ProtoObject* v = a->getAttribute(ctx, cc->valueKey);
        return v ? v : PROTO_NONE;
    }
    if (proto == cc->promiseMarkerProto) {
        // Busy-wait. protoCore's goUnmanaged tells the GC this thread
        // is parked on an OS-level wait so a STW can proceed without
        // it. Pair with returnFromUnmanaged before touching any
        // ProtoObject* again. The 1ms sleep amortises wake-up under
        // realistic deliver latencies.
        proto::ProtoThread* th = ctx->thread;
        for (;;) {
            const proto::ProtoObject* done =
                a->getAttribute(ctx, cc->doneKey);
            if (done == PROTO_TRUE) break;
            if (th) th->goUnmanaged();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (th) th->returnFromUnmanaged();
        }
        return a->getAttribute(ctx, cc->valueKey);
    }
    throw std::runtime_error("deref: not an atom, future, or promise");
}

// Session 18 — fire all registered watches on a successful atom
// mutation. The watches map lives under cc->watchesKey; each entry is
// `key → fn`. JVM-Clojure calls `(f key atom old-val new-val)` —
// same signature.
static void fireWatches(proto::ProtoContext* ctx,
                        const ActiveCallContext* cc,
                        const proto::ProtoObject* atomObj,
                        const proto::ProtoObject* oldV,
                        const proto::ProtoObject* newV) {
    const proto::ProtoObject* watchesObj =
        atomObj->getAttribute(ctx, cc->watchesKey);
    if (!watchesObj || watchesObj == PROTO_NONE) return;
    if (!isMap(ctx, mapLayoutOf(cc), watchesObj)) return;
    // Watches fire in the order they were added (map insertion order).
    struct Acc {
        const ActiveCallContext* cc;
        const proto::ProtoObject* atomObj;
        const proto::ProtoObject* oldV;
        const proto::ProtoObject* newV;
    } acc{cc, atomObj, oldV, newV};
    mapForEach(ctx, mapLayoutOf(cc), watchesObj, &acc,
        [](proto::ProtoContext* c, void* self,
           const proto::ProtoObject* k, const proto::ProtoObject* f) {
            auto* a = static_cast<Acc*>(self);
            const proto::ProtoObject* four[4] = {
                k, a->atomObj,
                a->oldV ? a->oldV : PROTO_NONE,
                a->newV ? a->newV : PROTO_NONE
            };
            try {
                a->cc->engine->invoke(c, f, four, 4);
            } catch (...) {
                // v0.18: swallow exceptions in a watcher; capturing
                // them is a follow-up.
            }
        });
}

const proto::ProtoObject* prim_reset_bang(proto::ProtoContext* ctx,
                                          const proto::ProtoObject*,
                                          const proto::ParentLink*,
                                          const proto::ProtoList* args,
                                          const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 2)
        throw std::runtime_error("reset!: expects (reset! atom new-value)");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throw std::runtime_error("reset!: no active VM context");
    const proto::ProtoObject* a = args->getAt(ctx, 0);
    if (!a || a->getPrototype(ctx) != cc->atomMarkerProto)
        throw std::runtime_error("reset!: not an atom");
    const proto::ProtoObject* nv = args->getAt(ctx, 1);
    const proto::ProtoObject* old = a->getAttribute(ctx, cc->valueKey);
    const_cast<proto::ProtoObject*>(a)
        ->setAttribute(ctx, cc->valueKey, nv);
    fireWatches(ctx, cc, a, old, nv);
    return nv;
}

const proto::ProtoObject* prim_swap_bang(proto::ProtoContext* ctx,
                                         const proto::ProtoObject*,
                                         const proto::ParentLink*,
                                         const proto::ProtoList* args,
                                         const proto::ProtoSparseList*) {
    unsigned long n = args ? args->getSize(ctx) : 0;
    if (n < 2)
        throw std::runtime_error("swap!: expects (swap! atom f & args)");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throw std::runtime_error("swap!: no active VM context");
    const proto::ProtoObject* a = args->getAt(ctx, 0);
    if (!a || a->getPrototype(ctx) != cc->atomMarkerProto)
        throw std::runtime_error("swap!: not an atom");
    const proto::ProtoObject* f = args->getAt(ctx, 1);

    // Build (old + extras) buffer once; old gets overwritten per attempt.
    unsigned long extras = n - 2;
    const proto::ProtoObject* buf[17];
    if (extras > 15)
        throw std::runtime_error("swap!: >15 extra args not supported");
    for (unsigned long i = 0; i < extras; ++i) {
        buf[i + 1] = args->getAt(ctx, static_cast<int>(2 + i));
    }

    // CAS retry loop. protoCore guarantees `getOwnAttributeDirect`
    // observes the value through the current shard root; if a
    // concurrent writer installs a new snapshot before our CAS
    // completes, setAttributeIfEqual returns false and we recompute
    // (f old ...) against the fresh `old`. Lock-free, no mutex.
    for (;;) {
        const proto::ProtoObject* old =
            a->getOwnAttributeDirect(ctx, cc->valueKey);
        buf[0] = old ? old : PROTO_NONE;
        const proto::ProtoObject* neu =
            cc->engine->invoke(ctx, f, buf, extras + 1);
        if (a->setAttributeIfEqual(ctx, cc->valueKey, old, neu)) {
            fireWatches(ctx, cc, a, old, neu);
            return neu;
        }
    }
}

// Session 17 — futures + pmap.
//
// The wire shape of a future:
//   getPrototype()           == futureMarkerProto
//   __thunk__                the 0-arg fn that produces the result.
//   __cc_blob__              long-encoded pointer to the parent's
//                            ActiveCallContext (the ExecutionEngine
//                            handle + every marker / key the worker
//                            thread needs to resume execution).
//   __thread__               long-encoded pointer to the ProtoThread.
//   __result__               the eventual value (initially nil).
//   __done__                 PROTO_FALSE until realized; then PROTO_TRUE.
//
// The thread main runs `futureThreadMain` below. It re-installs the
// parent's ActiveCallContext on its own TLS, invokes the thunk, and
// stores the result on the future via setAttribute. The compiler
// rewrites `(future body...)` into `(make-future (fn [] body...))`,
// so the thunk is just a regular user fn — it captures the
// surrounding lexical scope and the engine knows how to dispatch it.

// The worker reads its target future from args[0]: protoCore's
// newThread takes a ProtoList of "positional arguments for the
// target method", so the make-future caller stores the freshly
// allocated future as the single arg, and the worker pulls it back
// out of args via getAt.
const proto::ProtoObject* futureThreadMain(
        proto::ProtoContext* ctx,
        const proto::ProtoObject* /*self*/,
        const proto::ParentLink*,
        const proto::ProtoList* args,
        const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) == 0) return PROTO_NONE;
    const proto::ProtoObject* fut = args->getAt(ctx, 0);
    if (!fut) return PROTO_NONE;

    // Re-acquire the keys via createSymbol — cheap, interned.
    const proto::ProtoString* thunkKey  =
        proto::ProtoString::createSymbol(ctx, "__thunk__");
    const proto::ProtoString* ccBlobKey =
        proto::ProtoString::createSymbol(ctx, "__cc_blob__");
    const proto::ProtoString* resultKey =
        proto::ProtoString::createSymbol(ctx, "__result__");
    const proto::ProtoString* doneKey   =
        proto::ProtoString::createSymbol(ctx, "__done__");

    const proto::ProtoObject* thunk =
        fut->getAttribute(ctx, thunkKey);
    const proto::ProtoObject* blobObj =
        fut->getAttribute(ctx, ccBlobKey);
    if (!thunk || !blobObj || !blobObj->isInteger(ctx)) return PROTO_NONE;

    const ActiveCallContext* parentCc =
        reinterpret_cast<const ActiveCallContext*>(blobObj->asLong(ctx));
    if (!parentCc) return PROTO_NONE;

    // Install the parent's ActiveCallContext on this thread's TLS so
    // engine->invoke can dispatch through the same shared
    // ExecutionEngine + same globals + markers as the spawning thread.
    setActiveCallContext(*parentCc);

    const proto::ProtoObject* value = PROTO_NONE;
    try {
        value = parentCc->engine->invoke(ctx, thunk, nullptr, 0);
    } catch (...) {
        // v0.17: swallow exceptions, the future's result stays nil.
        // Capturing the throwable and re-raising on deref lands later.
        value = PROTO_NONE;
    }

    proto::ProtoObject* futMut =
        const_cast<proto::ProtoObject*>(fut);
    futMut->setAttribute(ctx, resultKey, value);
    futMut->setAttribute(ctx, doneKey, PROTO_TRUE);

    clearActiveCallContext();
    return value;
}

// Process-wide registry of every ProtoThread spawned by `(future …)`.
// Joined en masse by `shutdownFutures` before ProtoSpace destructs —
// the original session-17 implementation relied on the ProtoSpace
// destructor cleaning up worker threads, but if a worker was still
// dispatching VM code when the destructor ran it dereferenced
// already-freed Cells and the process crashed (`exit=139` on a tight
// `(future …)`-without-`@` loop; reproduced 10/10 with 50 in-flight
// futures, 2026-06-14). Joining every worker before exit is the same
// pattern `ActorScheduler::shutdown` uses.
namespace {
    std::mutex g_futureRegistryMtx;
    std::vector<const proto::ProtoThread*> g_futureThreads;
}

// Build a future, store the thunk + parent context, spawn the
// worker. The worker receives the future as its single positional
// argument (newThread's `args` slot 0).
static const proto::ProtoObject* buildFuture(
        proto::ProtoContext* ctx,
        const ActiveCallContext* cc,
        const proto::ProtoObject* thunk) {
    proto::ProtoObject* fut = const_cast<proto::ProtoObject*>(
        cc->futureMarkerProto->newChild(ctx, /*isMutable=*/true));
    fut->setAttribute(ctx, cc->thunkKey, thunk);
    fut->setAttribute(ctx, cc->ccBlobKey,
        ctx->fromLong(reinterpret_cast<long long>(cc)));
    fut->setAttribute(ctx, cc->doneKey, PROTO_FALSE);
    fut->setAttribute(ctx, cc->resultKey, PROTO_NONE);

    const proto::ProtoString* threadName =
        proto::ProtoString::createSymbol(ctx, "protoclj-future");
    const proto::ProtoList* targs =
        ctx->newList()->appendLast(ctx, fut);
    const proto::ProtoThread* thread =
        ctx->space->newThread(ctx, threadName, &futureThreadMain,
                              targs, nullptr);
    fut->setAttribute(ctx, cc->threadKey,
        ctx->fromLong(reinterpret_cast<long long>(thread)));
    {
        std::lock_guard<std::mutex> g(g_futureRegistryMtx);
        g_futureThreads.push_back(thread);
    }
    return fut;
}

// shutdownFutures (the externally-visible entry point) is defined
// below, *outside* the anonymous namespace this code is in.
static void shutdownFuturesImpl(proto::ProtoContext* ctx) {
    // Move the registry contents out under the lock so we don't hold
    // the mutex while joining (the worker thread will not register
    // anything new at exit time, but a slow `(future …)` invocation
    // racing against shutdown might — taking the lock here makes
    // shutdownFutures safe to call from any thread).
    std::vector<const proto::ProtoThread*> snapshot;
    {
        std::lock_guard<std::mutex> g(g_futureRegistryMtx);
        snapshot.swap(g_futureThreads);
    }
    // Blocking wait: run unmanaged so a still-running future can complete
    // a GC cycle it requests.
    proto::ProtoContext::UnmanagedScope unmanaged(ctx);
    for (const proto::ProtoThread* t : snapshot) {
        if (t) const_cast<proto::ProtoThread*>(t)->join(ctx);
    }
}

const proto::ProtoObject* prim_make_future(proto::ProtoContext* ctx,
                                           const proto::ProtoObject*,
                                           const proto::ParentLink*,
                                           const proto::ProtoList* args,
                                           const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 1)
        throw std::runtime_error("future: expects 1 thunk");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throw std::runtime_error("future: no active VM context");
    return buildFuture(ctx, cc, args->getAt(ctx, 0));
}

const proto::ProtoObject* prim_future_p(proto::ProtoContext* ctx,
                                        const proto::ProtoObject*,
                                        const proto::ParentLink*,
                                        const proto::ProtoList* args,
                                        const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 1)
        throw std::runtime_error("future?: expects 1 arg");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throw std::runtime_error("future?: no active VM context");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    if (!v) return PROTO_FALSE;
    return (v->getPrototype(ctx) == cc->futureMarkerProto) ? PROTO_TRUE : PROTO_FALSE;
}

const proto::ProtoObject* prim_realized_p(proto::ProtoContext* ctx,
                                          const proto::ProtoObject*,
                                          const proto::ParentLink*,
                                          const proto::ProtoList* args,
                                          const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 1)
        throw std::runtime_error("realized?: expects 1 arg");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throw std::runtime_error("realized?: no active VM context");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    if (!v) return PROTO_FALSE;
    const proto::ProtoObject* proto = v->getPrototype(ctx);
    if (proto != cc->futureMarkerProto && proto != cc->promiseMarkerProto)
        return PROTO_FALSE;
    const proto::ProtoObject* done = v->getAttribute(ctx, cc->doneKey);
    return done == PROTO_TRUE ? PROTO_TRUE : PROTO_FALSE;
}

// Session 18 — real parallel pmap.
//
// For each x in coll we build a future-shaped wrapper carrying f, x,
// and the parent's ActiveCallContext. The worker thread reads those
// from the wrapper, installs the cc on its TLS, and computes (f x)
// via engine->invoke. After spawning all N workers, the main thread
// joins each in order and collects results.

// Symbols used by pmapWorkerMain. Interned at first call; the
// SymbolTable caches them.
const proto::ProtoObject* pmapWorkerMain(
        proto::ProtoContext* ctx,
        const proto::ProtoObject* /*self*/,
        const proto::ParentLink*,
        const proto::ProtoList* args,
        const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) == 0) return PROTO_NONE;
    const proto::ProtoObject* fut = args->getAt(ctx, 0);
    if (!fut) return PROTO_NONE;

    const proto::ProtoString* fKey =
        proto::ProtoString::createSymbol(ctx, "__pmap_f__");
    const proto::ProtoString* xKey =
        proto::ProtoString::createSymbol(ctx, "__pmap_x__");
    const proto::ProtoString* ccBlobKey =
        proto::ProtoString::createSymbol(ctx, "__cc_blob__");
    const proto::ProtoString* resultKey =
        proto::ProtoString::createSymbol(ctx, "__result__");
    const proto::ProtoString* doneKey =
        proto::ProtoString::createSymbol(ctx, "__done__");

    const proto::ProtoObject* f = fut->getAttribute(ctx, fKey);
    const proto::ProtoObject* x = fut->getAttribute(ctx, xKey);
    const proto::ProtoObject* blobObj = fut->getAttribute(ctx, ccBlobKey);
    if (!f || !blobObj || !blobObj->isInteger(ctx)) return PROTO_NONE;
    const ActiveCallContext* parentCc =
        reinterpret_cast<const ActiveCallContext*>(blobObj->asLong(ctx));
    if (!parentCc) return PROTO_NONE;

    setActiveCallContext(*parentCc);

    const proto::ProtoObject* value = PROTO_NONE;
    try {
        const proto::ProtoObject* one[1] = { x };
        value = parentCc->engine->invoke(ctx, f, one, 1);
    } catch (...) {
        value = PROTO_NONE;
    }
    proto::ProtoObject* futMut = const_cast<proto::ProtoObject*>(fut);
    futMut->setAttribute(ctx, resultKey, value);
    futMut->setAttribute(ctx, doneKey, PROTO_TRUE);
    clearActiveCallContext();
    return value;
}

const proto::ProtoObject* prim_pmap(proto::ProtoContext* ctx,
                                    const proto::ProtoObject*,
                                    const proto::ParentLink*,
                                    const proto::ProtoList* args,
                                    const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 2)
        throw std::runtime_error("pmap: expects (pmap f coll)");
    const proto::ProtoObject* f    = args->getAt(ctx, 0);
    const proto::ProtoObject* coll = args->getAt(ctx, 1);
    const proto::ProtoList* lst = asSeqOrNull(ctx, coll);
    if (!lst) return ctx->newList()->asObject(ctx);
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throw std::runtime_error("pmap: no active VM context");
    unsigned long n = lst->getSize(ctx);

    const proto::ProtoString* fKey =
        proto::ProtoString::createSymbol(ctx, "__pmap_f__");
    const proto::ProtoString* xKey =
        proto::ProtoString::createSymbol(ctx, "__pmap_x__");
    const proto::ProtoString* threadName =
        proto::ProtoString::createSymbol(ctx, "protoclj-pmap");

    // Phase 1 — spawn all N workers. Collect the future wrappers
    // into a transient ProtoList so the GC can trace them.
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(1);
    constexpr unsigned int kSlotFs = 0;
    scope.setAutomaticLocal(kSlotFs,
        scope.newList()->asObject(&scope));

    for (unsigned long i = 0; i < n; ++i) {
        const proto::ProtoObject* x = lst->getAt(ctx, (int)i);
        proto::ProtoObject* fut = const_cast<proto::ProtoObject*>(
            cc->futureMarkerProto->newChild(ctx, /*isMutable=*/true));
        fut->setAttribute(ctx, fKey, f);
        fut->setAttribute(ctx, xKey, x);
        fut->setAttribute(ctx, cc->ccBlobKey,
            ctx->fromLong(reinterpret_cast<long long>(cc)));
        fut->setAttribute(ctx, cc->doneKey, PROTO_FALSE);
        fut->setAttribute(ctx, cc->resultKey, PROTO_NONE);

        const proto::ProtoList* targs =
            ctx->newList()->appendLast(ctx, fut);
        const proto::ProtoThread* t =
            ctx->space->newThread(ctx, threadName, &pmapWorkerMain,
                                  targs, nullptr);
        fut->setAttribute(ctx, cc->threadKey,
            ctx->fromLong(reinterpret_cast<long long>(t)));
        scope.setAutomaticLocal(kSlotFs,
            scope.getAutomaticLocal(kSlotFs)->asList(&scope)
                ->appendLast(&scope, fut)->asObject(&scope));
    }

    // Phase 2 — join in order, collect results.
    const proto::ProtoObject* out = ctx->newList()->asObject(ctx);
    const proto::ProtoList* fs =
        scope.getAutomaticLocal(kSlotFs)->asList(&scope);
    for (unsigned long i = 0; i < n; ++i) {
        const proto::ProtoObject* fut = fs->getAt(&scope, (int)i);
        const proto::ProtoObject* done =
            fut->getAttribute(ctx, cc->doneKey);
        if (done != PROTO_TRUE) {
            const proto::ProtoObject* tObj =
                fut->getAttribute(ctx, cc->threadKey);
            if (tObj && tObj->isInteger(ctx)) {
                proto::ProtoThread* t =
                    reinterpret_cast<proto::ProtoThread*>(tObj->asLong(ctx));
                if (t) {
                    // Blocking wait: run unmanaged so the worker can
                    // complete a GC cycle it requests.
                    proto::ProtoContext::UnmanagedScope unmanaged(ctx);
                    t->join(ctx);
                }
            }
        }
        const proto::ProtoObject* r =
            fut->getAttribute(ctx, cc->resultKey);
        out = out->asList(ctx)->appendLast(ctx,
            r ? r : PROTO_NONE)->asObject(ctx);
    }
    return out;
}

// Session 18 — promises. A promise is a child of promiseMarkerProto
// with `__value__` initially nil and `__done__` PROTO_FALSE. deliver
// CAS-installs the value once; subsequent delivers are no-ops and
// return nil. deref busy-waits on __done__ — see prim_deref above.

const proto::ProtoObject* prim_promise(proto::ProtoContext* ctx,
                                       const proto::ProtoObject*,
                                       const proto::ParentLink*,
                                       const proto::ProtoList* args,
                                       const proto::ProtoSparseList*) {
    if (args && args->getSize(ctx) != 0)
        throw std::runtime_error("promise: takes no arguments");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throw std::runtime_error("promise: no active VM context");
    proto::ProtoObject* p = const_cast<proto::ProtoObject*>(
        cc->promiseMarkerProto->newChild(ctx, /*isMutable=*/true));
    p->setAttribute(ctx, cc->valueKey, PROTO_NONE);
    p->setAttribute(ctx, cc->doneKey, PROTO_FALSE);
    return p;
}

const proto::ProtoObject* prim_promise_p(proto::ProtoContext* ctx,
                                         const proto::ProtoObject*,
                                         const proto::ParentLink*,
                                         const proto::ProtoList* args,
                                         const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 1)
        throw std::runtime_error("promise?: expects 1 arg");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throw std::runtime_error("promise?: no active VM context");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    if (!v) return PROTO_FALSE;
    return (v->getPrototype(ctx) == cc->promiseMarkerProto) ? PROTO_TRUE : PROTO_FALSE;
}

// Session 19 — actors.
//
// (actor initial-state) builds a mutable wrapper child of
// actorMarkerProto with `__value__` = initial-state and
// `__actor_state__` = long-encoded pointer to an ActorState struct
// owned by the global ActorScheduler. The scheduler is started
// lazily on the first actor allocation.
//
// (send actor f & args) enqueues `(f current-state args...)` on the
// actor's mailbox at MEDIUM priority and returns a promise that
// resolves to the new state when the message is processed.
//
// (send-h …) / (send-m …) / (send-l …) — explicit High / Medium /
// Low priority.
//
// (actor? x) — prototype check.
// @actor — read the current state through the standard deref path.

static ActorState* actorStateFrom(proto::ProtoContext* ctx,
                                  const ActiveCallContext* cc,
                                  const proto::ProtoObject* v,
                                  const char* primName) {
    if (!v || v->getPrototype(ctx) != cc->actorMarkerProto)
        throw std::runtime_error(std::string(primName) + ": not an actor");
    const proto::ProtoObject* h = v->getAttribute(ctx, cc->actorStateKey);
    if (!h || !h->isInteger(ctx))
        throw std::runtime_error(std::string(primName) + ": broken actor handle");
    return reinterpret_cast<ActorState*>(h->asLong(ctx));
}

const proto::ProtoObject* prim_actor(proto::ProtoContext* ctx,
                                     const proto::ProtoObject*,
                                     const proto::ParentLink*,
                                     const proto::ProtoList* args,
                                     const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 1)
        throw std::runtime_error("actor: expects (actor initial-state)");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throw std::runtime_error("actor: no active VM context");

    auto& sched = ActorScheduler::instance();
    sched.ensureStarted(ctx->space, ctx, *cc);

    const proto::ProtoObject* init = args->getAt(ctx, 0);
    proto::ProtoObject* wrap = const_cast<proto::ProtoObject*>(
        cc->actorMarkerProto->newChild(ctx, /*isMutable=*/true));
    wrap->setAttribute(ctx, cc->valueKey, init);

    ActorState* state = sched.newActor(wrap, init);
    wrap->setAttribute(ctx, cc->actorStateKey,
        ctx->fromLong(reinterpret_cast<long long>(state)));
    return wrap;
}

const proto::ProtoObject* prim_actor_p(proto::ProtoContext* ctx,
                                       const proto::ProtoObject*,
                                       const proto::ParentLink*,
                                       const proto::ProtoList* args,
                                       const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 1)
        throw std::runtime_error("actor?: expects 1 arg");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throw std::runtime_error("actor?: no active VM context");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    if (!v) return PROTO_FALSE;
    return (v->getPrototype(ctx) == cc->actorMarkerProto)
           ? PROTO_TRUE : PROTO_FALSE;
}

// Shared core for send / send-h / send-m / send-l.
static const proto::ProtoObject* sendCore(proto::ProtoContext* ctx,
                                           const proto::ProtoList* args,
                                           const char* primName,
                                           ActorPriority priority) {
    if (!args || args->getSize(ctx) < 2)
        throw std::runtime_error(std::string(primName) + ": expects (… actor f & args)");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throw std::runtime_error(std::string(primName) + ": no active VM context");
    ActorState* state = actorStateFrom(ctx, cc, args->getAt(ctx, 0), primName);
    const proto::ProtoObject* f = args->getAt(ctx, 1);

    // Pack the extra args (positions 2..N-1) into a fresh ProtoList.
    const proto::ProtoObject* extras = ctx->newList()->asObject(ctx);
    unsigned long n = args->getSize(ctx);
    for (unsigned long i = 2; i < n; ++i) {
        extras = extras->asList(ctx)
            ->appendLast(ctx, args->getAt(ctx, (int)i))->asObject(ctx);
    }

    // Build the per-send promise — same wire shape as `promise`.
    proto::ProtoObject* p = const_cast<proto::ProtoObject*>(
        cc->promiseMarkerProto->newChild(ctx, /*isMutable=*/true));
    p->setAttribute(ctx, cc->valueKey, PROTO_NONE);
    p->setAttribute(ctx, cc->doneKey, PROTO_FALSE);

    ActorMessage msg{f, extras, p, priority};
    ActorScheduler::instance().send(state, std::move(msg));
    return p;
}

const proto::ProtoObject* prim_send(proto::ProtoContext* ctx,
                                    const proto::ProtoObject*,
                                    const proto::ParentLink*,
                                    const proto::ProtoList* args,
                                    const proto::ProtoSparseList*) {
    return sendCore(ctx, args, "send", ActorPriority::Medium);
}

const proto::ProtoObject* prim_send_h(proto::ProtoContext* ctx,
                                      const proto::ProtoObject*,
                                      const proto::ParentLink*,
                                      const proto::ProtoList* args,
                                      const proto::ProtoSparseList*) {
    return sendCore(ctx, args, "send-h", ActorPriority::High);
}

const proto::ProtoObject* prim_send_m(proto::ProtoContext* ctx,
                                      const proto::ProtoObject*,
                                      const proto::ParentLink*,
                                      const proto::ProtoList* args,
                                      const proto::ProtoSparseList*) {
    return sendCore(ctx, args, "send-m", ActorPriority::Medium);
}

const proto::ProtoObject* prim_send_l(proto::ProtoContext* ctx,
                                      const proto::ProtoObject*,
                                      const proto::ParentLink*,
                                      const proto::ProtoList* args,
                                      const proto::ProtoSparseList*) {
    return sendCore(ctx, args, "send-l", ActorPriority::Low);
}

// (actor-stats) — diagnostics, returns a small map.
const proto::ProtoObject* prim_actor_stats(proto::ProtoContext* ctx,
                                           const proto::ProtoObject*,
                                           const proto::ParentLink*,
                                           const proto::ProtoList*,
                                           const proto::ProtoSparseList*) {
    auto s = ActorScheduler::instance().stats();
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throw std::runtime_error("actor-stats: no active VM context");
    // Counts are SmallInts and keywords are interned (reachable through the
    // intern table, Named.h): nothing here needs rooting before the map is
    // built.
    const proto::ProtoObject* kv[4] = {
        internNamed(ctx, cc->named, ":workers"),
        ctx->fromLong(s.numWorkers),
        internNamed(ctx, cc->named, ":messages-processed"),
        ctx->fromLong(static_cast<long long>(s.messagesProcessed))};
    return mapAssocPairs(ctx, mapLayoutOf(cc), nullptr, kv, 4);
}

const proto::ProtoObject* prim_deliver(proto::ProtoContext* ctx,
                                       const proto::ProtoObject*,
                                       const proto::ParentLink*,
                                       const proto::ProtoList* args,
                                       const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 2)
        throw std::runtime_error("deliver: expects (deliver promise value)");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throw std::runtime_error("deliver: no active VM context");
    const proto::ProtoObject* p = args->getAt(ctx, 0);
    if (!p || p->getPrototype(ctx) != cc->promiseMarkerProto)
        throw std::runtime_error("deliver: not a promise");
    const proto::ProtoObject* v = args->getAt(ctx, 1);
    // Single-shot CAS on doneKey: succeed only when still PROTO_FALSE.
    bool first = p->setAttributeIfEqual(ctx, cc->doneKey, PROTO_FALSE, PROTO_TRUE);
    if (!first) return PROTO_NONE;
    const_cast<proto::ProtoObject*>(p)->setAttribute(ctx, cc->valueKey, v);
    return p;
}

const proto::ProtoObject* prim_compare_and_set_bang(
        proto::ProtoContext* ctx,
        const proto::ProtoObject*,
        const proto::ParentLink*,
        const proto::ProtoList* args,
        const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 3)
        throw std::runtime_error("compare-and-set!: expects (compare-and-set! atom old new)");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throw std::runtime_error("compare-and-set!: no active VM context");
    const proto::ProtoObject* a = args->getAt(ctx, 0);
    if (!a || a->getPrototype(ctx) != cc->atomMarkerProto)
        throw std::runtime_error("compare-and-set!: not an atom");
    const proto::ProtoObject* expected = args->getAt(ctx, 1);
    const proto::ProtoObject* nv       = args->getAt(ctx, 2);
    bool ok = a->setAttributeIfEqual(ctx, cc->valueKey, expected, nv);
    if (ok) fireWatches(ctx, cc, a, expected, nv);
    return ok ? PROTO_TRUE : PROTO_FALSE;
}

const proto::ProtoObject* prim_add_watch(proto::ProtoContext* ctx,
                                         const proto::ProtoObject*,
                                         const proto::ParentLink*,
                                         const proto::ProtoList* args,
                                         const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 3)
        throw std::runtime_error("add-watch: expects (add-watch atom key fn)");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throw std::runtime_error("add-watch: no active VM context");
    const proto::ProtoObject* a = args->getAt(ctx, 0);
    if (!a || a->getPrototype(ctx) != cc->atomMarkerProto)
        throw std::runtime_error("add-watch: not an atom");
    const proto::ProtoObject* k = args->getAt(ctx, 1);
    const proto::ProtoObject* f = args->getAt(ctx, 2);

    // CAS the watches map (key -> fn). `neu` is rooted in an automatic
    // local across the CAS, which allocates.
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(1);
    const proto::ProtoObject* kf[2] = {k, f};
    for (;;) {
        const proto::ProtoObject* old =
            a->getOwnAttributeDirect(&scope, cc->watchesKey);
        const proto::ProtoObject* base =
            isMap(&scope, mapLayoutOf(cc), old) ? old : nullptr;
        scope.setAutomaticLocal(0,
            mapAssocPairs(&scope, mapLayoutOf(cc), base, kf, 2));
        if (a->setAttributeIfEqual(&scope, cc->watchesKey, old,
                                   scope.getAutomaticLocal(0))) break;
    }
    return a;
}

const proto::ProtoObject* prim_remove_watch(proto::ProtoContext* ctx,
                                            const proto::ProtoObject*,
                                            const proto::ParentLink*,
                                            const proto::ProtoList* args,
                                            const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) != 2)
        throw std::runtime_error("remove-watch: expects (remove-watch atom key)");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throw std::runtime_error("remove-watch: no active VM context");
    const proto::ProtoObject* a = args->getAt(ctx, 0);
    if (!a || a->getPrototype(ctx) != cc->atomMarkerProto)
        throw std::runtime_error("remove-watch: not an atom");
    const proto::ProtoObject* k = args->getAt(ctx, 1);

    // CAS the watches map without `k`. mapDissoc keeps the order of the
    // remaining watches; `neu` is rooted across the CAS, which allocates.
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(1);
    for (;;) {
        const proto::ProtoObject* old =
            a->getOwnAttributeDirect(&scope, cc->watchesKey);
        if (!isMap(&scope, mapLayoutOf(cc), old)) return a;
        scope.setAutomaticLocal(0, mapDissoc(&scope, mapLayoutOf(cc), old, k));
        if (scope.getAutomaticLocal(0) == old) return a;   // key absent
        if (a->setAttributeIfEqual(&scope, cc->watchesKey, old,
                                   scope.getAutomaticLocal(0))) break;
    }
    return a;
}

// Every built-in function and the global name it is installed under. The
// single source for installPrimitives and for the printer's `#<fn NAME>`.
struct PrimitiveEntry {
    const char*       name;
    proto::ProtoMethod fn;
};

constexpr PrimitiveEntry kPrimitives[] = {
    {"println", &prim_println},
    {"+",       &prim_plus},
    {"-",       &prim_minus},
    {"*",       &prim_mul},
    {"inc",     &prim_inc},
    {"dec",     &prim_dec},
    {"<",       &prim_lt},
    {"<=",      &prim_le},
    {">",       &prim_gt},
    {">=",      &prim_ge},
    {"=",       &prim_eq},
    {"not=",    &prim_not_eq},
    {"str",     &prim_str},
    {"/",       &prim_div},

    // Lists, vectors and predicates.
    {"list",    &prim_list},
    {"vector",  &prim_vector},
    {"vec",     &prim_vec},
    {"nth",     &prim_nth},
    {"vector?", &prim_vector_p},
    {"list?",   &prim_list_p},

    // Maps.
    {"hash-map",  &prim_hash_map},
    {"assoc",     &prim_assoc},
    {"get",       &prim_get},
    {"contains?", &prim_contains_p},
    {"keys",      &prim_keys},
    {"vals",      &prim_vals},
    {"map?",      &prim_map_p},

    // String ops (clojure.string-shaped, in the global namespace since
    // `ns` does not exist yet).
    {"string?",      &prim_string_p},
    {"subs",         &prim_subs},
    {"upper-case",   &prim_upper_case},
    {"lower-case",   &prim_lower_case},
    {"starts-with?", &prim_starts_with_p},
    {"ends-with?",   &prim_ends_with_p},
    {"includes?",    &prim_includes_p},
    {"index-of",     &prim_index_of},
    {"replace",      &prim_replace},
    {"join",         &prim_join},
    {"split",        &prim_split},
    {"trim",         &prim_trim},
    {"triml",        &prim_triml},
    {"trimr",        &prim_trimr},
    {"blank?",       &prim_blank_p},

    // Atoms.
    {"atom",             &prim_atom},
    {"atom?",            &prim_atom_p},
    {"deref",            &prim_deref},
    {"reset!",           &prim_reset_bang},
    {"swap!",            &prim_swap_bang},
    {"compare-and-set!", &prim_compare_and_set_bang},

    // Futures and pmap.
    {"make-future", &prim_make_future},
    {"future?",     &prim_future_p},
    {"realized?",   &prim_realized_p},
    {"pmap",        &prim_pmap},

    // Watches and promises.
    {"add-watch",    &prim_add_watch},
    {"remove-watch", &prim_remove_watch},
    {"promise",      &prim_promise},
    {"promise?",     &prim_promise_p},
    {"deliver",      &prim_deliver},

    // Actors.
    {"actor",       &prim_actor},
    {"actor?",      &prim_actor_p},
    {"send",        &prim_send},
    {"send-h",      &prim_send_h},
    {"send-m",      &prim_send_m},
    {"send-l",      &prim_send_l},
    {"actor-stats", &prim_actor_stats},

    // Sequences and higher-order functions.
    {"first",   &prim_first},
    {"rest",    &prim_rest},
    {"cons",    &prim_cons},
    {"count",   &prim_count},
    {"empty?",  &prim_empty_p},
    {"nil?",    &prim_nil_p},
    {"not",     &prim_not},
    {"reverse", &prim_reverse},
    {"map",     &prim_map},
    {"filter",  &prim_filter},
    {"reduce",  &prim_reduce},
};

// A linear scan of the 75 entries; printing a function is not a hot path.
const char* primitiveName(proto::ProtoMethod fn) {
    for (const PrimitiveEntry& p : kPrimitives) {
        if (p.fn == fn) return p.name;
    }
    return nullptr;
}

} // namespace

// Externally-visible shutdownFutures — declared in Primitives.h, lives
// outside the anonymous namespace so the linker can find it from
// main.cpp. Delegates to the static impl above which has access to the
// registry inside the anonymous namespace.
void shutdownFutures(proto::ProtoContext* ctx) {
    shutdownFuturesImpl(ctx);
}

// Externally-visible Clojure-shape value printer. Same TU as the
// anonymous-namespace `printValue`; the implicit using-directive that
// `namespace {}` introduces makes that symbol reachable from here
// (it has internal linkage but is in scope within this TU), so we can
// just delegate.
void replPrintValue(proto::ProtoContext* ctx, std::FILE* out,
                    const proto::ProtoObject* v) {
    printValue(ctx, out, v, /*readable=*/true);
}

namespace {

// A list or a vector read through one indexed interface, so the sequential
// rules below treat both concrete types alike. Every sequence the runtime
// produces (`rest`, `map`, `filter`, `keys`, `cons`, ...) is one of the two.
struct SequentialView {
    const proto::ProtoList*  list  = nullptr;
    const proto::ProtoTuple* tuple = nullptr;

    explicit operator bool() const { return list || tuple; }
    unsigned long size(proto::ProtoContext* ctx) const {
        return list ? list->getSize(ctx) : tuple->getSize(ctx);
    }
    // O(log n): protoCore exposes no allocation-free sequential walk of a
    // ProtoList or a ProtoTuple, so elements are read by index.
    const proto::ProtoObject* at(proto::ProtoContext* ctx, unsigned long i) const {
        const int idx = static_cast<int>(i);
        return list ? list->getAt(ctx, idx) : tuple->getAt(ctx, idx);
    }
};

SequentialView sequentialView(proto::ProtoContext* ctx,
                              const proto::ProtoObject* v) {
    SequentialView s;
    if (isListTag(v)) s.list = v->asList(ctx);
    else              s.tuple = asTupleOrNull(ctx, v);
    return s;
}

// --- valueHash helpers --------------------------------------------------
//
// A number hashes to its value modulo the Mersenne prime 2^61 - 1, the
// scheme CPython uses for int and float. protoCore compare equates an
// integer and a double only when they are mathematically equal, and equal
// values have equal residues, so 1 and 1.0, or 2^70 as a LargeInteger and
// as a double, hash equally (deviation D15). A negative value hashes to the
// two's complement of its magnitude's residue.

constexpr unsigned kHashBits = 61;
constexpr unsigned long long kHashModulus = (1ULL << kHashBits) - 1;
constexpr unsigned long long kHashInfinity = 314159;
constexpr unsigned long long kHashNaN = 0;
constexpr unsigned long long kSequentialSeed = 0x6A09E667F3BCC909ULL;
constexpr unsigned long long kMapSeed = 0xBB67AE8584CAA73BULL;

// splitmix64 finalizer: a bijective mix used to combine element hashes.
inline unsigned long long mix64(unsigned long long z) {
    z ^= z >> 30;
    z *= 0xBF58476D1CE4E5B9ULL;
    z ^= z >> 27;
    z *= 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

inline unsigned long long withSign(bool negative, unsigned long long residue) {
    return negative ? 0ULL - residue : residue;
}

// residue * 2^shift mod (2^61 - 1), for residue < 2^61 and shift < 61.
inline unsigned long long rotateResidue(unsigned long long residue,
                                        unsigned shift) {
    if (shift == 0) return residue;
    return ((residue << shift) & kHashModulus) |
           (residue >> (kHashBits - shift));
}

unsigned long long hashLong(long long v) {
    const bool negative = v < 0;
    const unsigned long long magnitude =
        negative ? 0ULL - static_cast<unsigned long long>(v)
                 : static_cast<unsigned long long>(v);
    return withSign(negative, magnitude % kHashModulus);
}

// Integers beyond the long long range: the residue of the hexadecimal
// digits. The digit string is protoCore's allocation (made under its own
// critical section) and is consumed before anything else allocates.
unsigned long long hashIntegerDigits(proto::ProtoContext* ctx,
                                     const proto::ProtoObject* v) {
    const std::string digits = v->asIntegerString(ctx, 16)->toStdString(ctx);
    bool negative = false;
    unsigned long long residue = 0;
    for (char c : digits) {
        if (c == '-') { negative = true; continue; }
        const unsigned long long digit = (c >= '0' && c <= '9')
            ? static_cast<unsigned long long>(c - '0')
            : static_cast<unsigned long long>(c - 'a' + 10);
        residue = rotateResidue(residue, 4) + digit;
        if (residue >= kHashModulus) residue -= kHashModulus;
    }
    return withSign(negative, residue);
}

unsigned long long hashInteger(proto::ProtoContext* ctx,
                               const proto::ProtoObject* v) {
    try {
        return hashLong(v->asLong(ctx));
    } catch (const std::overflow_error&) {
        return hashIntegerDigits(ctx, v);
    }
}

// The residue of mantissa * 2^exponent, taking the mantissa 28 bits at a
// time; 2^61 = 1 (mod 2^61 - 1), so the exponent only rotates the residue.
unsigned long long hashDouble(double v) {
    if (std::isnan(v)) return kHashNaN;
    if (std::isinf(v)) return withSign(v < 0, kHashInfinity);
    int exponent = 0;
    double mantissa = std::frexp(v, &exponent);
    const bool negative = mantissa < 0;
    if (negative) mantissa = -mantissa;
    unsigned long long residue = 0;
    while (mantissa != 0.0) {
        residue = rotateResidue(residue, 28);
        mantissa *= 268435456.0;  // 2^28
        exponent -= 28;
        const auto chunk = static_cast<unsigned long long>(mantissa);
        mantissa -= static_cast<double>(chunk);
        residue += chunk;
        if (residue >= kHashModulus) residue -= kHashModulus;
    }
    const int bits = static_cast<int>(kHashBits);
    const int shift = exponent >= 0 ? exponent % bits
                                    : bits - 1 - ((-1 - exponent) % bits);
    return withSign(negative, rotateResidue(residue, static_cast<unsigned>(shift)));
}

} // namespace

const char* valueTypeName(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
    if (!v || v == PROTO_NONE)              return "nil";
    if (v == PROTO_TRUE || v == PROTO_FALSE) return "a boolean";
    if (v->isInteger(ctx))                  return "an integer";
    if (v->isDouble(ctx))                   return "a float";
    if (proto::ProtoObject::isStringTagFast(v)) return "a string";
    if (isListTag(v))                       return "a list";
    if (v->isTuple(ctx))                    return "a vector";
    if (v->isMethod(ctx))                   return "a fn";
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) return "an object";
    if (isNamed(ctx, cc->named, v)) {
        // A keyword's spelling starts with ':', a symbol's never does.
        const std::string spelling =
            namedSpelling(ctx, cc->named, v)->toStdString(ctx);
        return (!spelling.empty() && spelling[0] == ':') ? "a keyword" : "a symbol";
    }
    const proto::ProtoObject* prototype = v->getPrototype(ctx);
    if (prototype == cc->mapMarkerProto)     return "a map";
    if (prototype == cc->atomMarkerProto)    return "an atom";
    if (prototype == cc->futureMarkerProto)  return "a future";
    if (prototype == cc->promiseMarkerProto) return "a promise";
    if (prototype == cc->actorMarkerProto)   return "an actor";
    if (prototype == cc->fnSingleProto || prototype == cc->fnMultiProto)
        return "a fn";
    return "an object";
}

void throwNotANumber(proto::ProtoContext* ctx, const char* operation,
                     const proto::ProtoObject* v) {
    throw std::runtime_error(std::string("ClassCastException: ") + operation +
                             " expects a number, got " + valueTypeName(ctx, v));
}

namespace {

// `m` (finite, positive) in the scientific notation of std::to_chars,
// "d[.ddd]e±XX": with the shortest digits that read back as `m` when
// `precision` is negative, else with `precision` fractional digits,
// correctly rounded.
std::string toCharsScientific(double m, int precision) {
    char buf[64];
    const std::to_chars_result r = precision < 0
        ? std::to_chars(buf, buf + sizeof buf, m, std::chars_format::scientific)
        : std::to_chars(buf, buf + sizeof buf, m, std::chars_format::scientific,
                        precision);
    return std::string(buf, r.ptr);
}

// The significant digits (without the point) and the decimal exponent of a
// toCharsScientific result.
void splitScientific(const std::string& s, std::string& digits, int& exponent) {
    const std::size_t e = s.find('e');
    digits.clear();
    for (std::size_t i = 0; i < e; ++i)
        if (s[i] != '.') digits += s[i];
    exponent = std::atoi(s.c_str() + e + 1);
}

} // namespace

std::string formatDouble(double d) {
    if (std::isnan(d)) return "##NaN";
    if (std::isinf(d)) return d > 0 ? "##Inf" : "##-Inf";
    std::string out = std::signbit(d) ? "-" : "";
    const double m = std::fabs(d);
    if (m == 0.0) return out + "0.0";

    // Java's Double.toString (JDK 19 and later) selects the shortest decimal
    // that rounds to the double; when that decimal has a single digit, it
    // takes the two-digit decimal closest to the double instead, provided it
    // still rounds to it (the smallest subnormal prints as 4.9E-324, not
    // 5.0E-324). Trailing zeros are not significant digits.
    std::string digits;
    int exponent = 0;
    splitScientific(toCharsScientific(m, -1), digits, exponent);
    if (digits.size() == 1) {
        const std::string two = toCharsScientific(m, 1);
        if (std::strtod(two.c_str(), nullptr) == m)
            splitScientific(two, digits, exponent);
    }
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();

    // Plain notation for magnitudes in [1e-3, 1e7), with at least one
    // fractional digit; otherwise <digit>.<digits>E<exponent>, with at least
    // one digit after the point.
    const int n = static_cast<int>(digits.size());
    if (exponent >= -3 && exponent <= 6) {
        if (exponent < 0) {
            out += "0.";
            out.append(static_cast<std::size_t>(-exponent - 1), '0');
            out += digits;
        } else if (n > exponent + 1) {
            out.append(digits, 0, static_cast<std::size_t>(exponent + 1));
            out += '.';
            out.append(digits, static_cast<std::size_t>(exponent + 1));
        } else {
            out += digits;
            out.append(static_cast<std::size_t>(exponent + 1 - n), '0');
            out += ".0";
        }
    } else {
        out += digits[0];
        out += '.';
        if (n > 1) out.append(digits, 1);
        else       out += '0';
        out += 'E';
        out += std::to_string(exponent);
    }
    return out;
}

// Externally-visible value equality, declared in Primitives.h; shared by
// `=` / `not=` and the VM's EQ opcode.
bool valuesEqual(proto::ProtoContext* ctx, const MapLayout& layout,
                 const proto::ProtoObject* a, const proto::ProtoObject* b) {
    if (!a) a = PROTO_NONE;
    if (!b) b = PROTO_NONE;
    if (a == b) return true;
    // Recurses once per level of nesting (StackGuard.h).
    checkNativeStack();

    const bool aMap = isMap(ctx, layout, a);
    const bool bMap = isMap(ctx, layout, b);
    if (aMap || bMap) {
        return aMap && bMap &&
            mapEquals(ctx, layout, a, b,
                const_cast<void*>(static_cast<const void*>(&layout)),
                [](proto::ProtoContext* c, void* self,
                   const proto::ProtoObject* x, const proto::ProtoObject* y) {
                    return valuesEqual(c, *static_cast<const MapLayout*>(self),
                                       x, y);
                });
    }

    // Any other object — keywords and symbols (interned, Named.h), atoms,
    // functions, ... — is equal only to itself. Decided by the pointer tag:
    // protoCore's isTuple, isString and compare would first probe the
    // object for a `__data__` wrapper attribute, which the runtime never sets.
    if (isObjectTag(a) || isObjectTag(b)) return false;

    // Sequential collections (lists and vectors) are equal when they hold
    // equal elements in the same order, whatever their concrete types, and
    // are never equal to anything else. One pass over the elements that
    // stops at the first mismatch. (protoCore shares equal tuples of
    // identical elements, which the pointer test above already accepts.)
    const SequentialView sa = sequentialView(ctx, a);
    const SequentialView sb = sequentialView(ctx, b);
    if (sa || sb) {
        if (!sa || !sb) return false;
        const unsigned long n = sa.size(ctx);
        if (n != sb.size(ctx)) return false;
        for (unsigned long i = 0; i < n; ++i) {
            if (!valuesEqual(ctx, layout, sa.at(ctx, i), sb.at(ctx, i)))
                return false;
        }
        return true;
    }

    return a->compare(ctx, b) == 0;
}

// Externally-visible value hash, declared in Primitives.h; the key hash of
// every map. Its categories mirror valuesEqual's.
unsigned long valueHash(proto::ProtoContext* ctx, const MapLayout& layout,
                        const proto::ProtoObject* v) {
    if (!v) v = PROTO_NONE;
    if (v->isInteger(ctx)) return hashInteger(ctx, v);
    if (v->isDouble(ctx))  return hashDouble(v->asDouble(ctx));
    if (proto::ProtoObject::isStringTagFast(v)) return v->getHash(ctx);
    // Recurses once per level of nesting (StackGuard.h).
    checkNativeStack();

    if (isMap(ctx, layout, v)) {
        // Order-independent: a sum of per-entry mixes. The key hashes are
        // the ones stored in the map's hash index (valueHash of each key).
        struct Acc {
            const MapLayout*   layout;
            unsigned long long sum;
            unsigned long long count;
        } acc{&layout, 0, 0};
        mapForEachHashedEntry(ctx, layout, v, &acc,
            [](proto::ProtoContext* c, void* self, unsigned long keyHash,
               const proto::ProtoObject* value) {
                auto* a = static_cast<Acc*>(self);
                a->sum += mix64(keyHash ^ mix64(valueHash(c, *a->layout, value)));
                ++a->count;
            });
        return mix64(acc.sum ^ mix64(kMapSeed + acc.count));
    }

    // Any other object is equal only to itself (valuesEqual): hash its
    // address, without the `__data__` probes of protoCore getHash.
    if (isObjectTag(v)) return mix64(reinterpret_cast<uintptr_t>(v));

    // Lists and vectors share one order-dependent combination, so equal
    // sequential collections hash equally whatever their concrete types.
    const SequentialView s = sequentialView(ctx, v);
    if (s) {
        const unsigned long n = s.size(ctx);
        unsigned long long h = kSequentialSeed;
        for (unsigned long i = 0; i < n; ++i)
            h = mix64(h + valueHash(ctx, layout, s.at(ctx, i)));
        return mix64(h ^ n);
    }

    // nil, booleans and primitive functions: protoCore's identity-based
    // hash, matching the identity comparison valuesEqual falls back to.
    return v->getHash(ctx);
}

void installPrimitives(proto::ProtoContext* ctx,
                       proto::ProtoObject* globals) {
    // Install each primitive: wrap the C function pointer in a callable
    // ProtoObject via fromMethod, store on the globals under the symbol key.
    // setAttribute on a mutable receiver mutates in place.
    for (const PrimitiveEntry& p : kPrimitives) {
        const proto::ProtoString* key =
            proto::ProtoString::createSymbol(ctx, p.name);
        const proto::ProtoObject* callable =
            ctx->fromMethod(nullptr /* self */, p.fn);
        globals->setAttribute(ctx, key, callable);
    }
}

} // namespace protoClojure
