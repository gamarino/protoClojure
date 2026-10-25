#include "Primitives.h"
#include "Utf8String.h"
#include "Exceptions.h"
#include "IO.h"
#include "CorePrims.h"
#include "DataFormats.h"
#include "LazySeq.h"
#include "DebugHook.h"
#include "ExecutionEngine.h"
#include "ActorScheduler.h"
#include "ListBuilder.h"
#include "MapOps.h"
#include "Macros.h"
#include "SetOps.h"
#include "Regex.h"
#include "VectorOps.h"
#include "StackGuard.h"

#include "protoCore.h"

#include <algorithm>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <memory>
#include <stdexcept>
#include <string>
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

// Argument `i` of an index or position parameter (nth, subs, index-of): an
// integer of any size. A LargeInteger beyond the long long range saturates
// to LLONG_MAX or LLONG_MIN, which every caller's bounds check treats as out
// of range; the caller's error prints the argument itself (printedValue),
// not the saturated value. Anything but an integer raises the
// ClassCastException analogue naming the primitive and the type.
long long indexArg(proto::ProtoContext* ctx, const proto::ProtoArguments* args,
                   int i, const char* primName) {
    const proto::ProtoObject* a = args->getAt(ctx, i);
    if (!a || !a->isInteger(ctx)) {
        throwClassed(ctx, "ClassCastException", std::string(primName) +
                                 " expects an integer, got " +
                                 valueTypeName(ctx, a));
    }
    constexpr proto::proto_ulong kSmallIntMask  = PROTO_UL(0x3FF);
    constexpr proto::proto_ulong kSmallIntValue = PROTO_UL(0x001);
    if ((reinterpret_cast<proto::proto_ulong>(a) & kSmallIntMask) == kSmallIntValue)
        return reinterpret_cast<long long>(a) >> 10;
    if (a->compare(ctx, ctx->fromLong(LLONG_MAX)) > 0) return LLONG_MAX;
    if (a->compare(ctx, ctx->fromLong(LLONG_MIN)) < 0) return LLONG_MIN;
    return a->asLong(ctx);
}

// Argument `i` of a numeric primitive: an integer (SmallInteger or
// LargeInteger) or a float. Anything else raises the ClassCastException
// analogue naming the primitive and the argument's type (throwNotANumber),
// the same error the arithmetic opcodes raise.
const proto::ProtoObject* numberArg(proto::ProtoContext* ctx,
                                    const proto::ProtoArguments* args,
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
    constexpr proto::proto_ulong kSmallIntMask  = PROTO_UL(0x3FF);
    constexpr proto::proto_ulong kSmallIntValue = PROTO_UL(0x001);
    if ((reinterpret_cast<proto::proto_ulong>(a) & kSmallIntMask) == kSmallIntValue &&
        (reinterpret_cast<proto::proto_ulong>(b) & kSmallIntMask) == kSmallIntValue) {
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

// The value delivered to `promise`, with `*delivered` set; nullptr, with
// `*delivered` false, while the promise is pending. One own-attribute read
// answers both, because deliverPromise installs the one-element list holding
// the value in a single compare-and-set.
const proto::ProtoObject* promiseValue(proto::ProtoContext* ctx,
                                       const proto::ProtoString* valueKey,
                                       const proto::ProtoObject* promise,
                                       bool* delivered) {
    const proto::ProtoObject* box = promise->getOwnAttributeDirect(ctx, valueKey);
    *delivered = box != nullptr;
    return box ? box->asList(ctx)->getAt(ctx, 0) : nullptr;
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
//   - lists `(1 2)`, vectors `[1 2]` and maps `{:a 1, :b 2}` (each entry's
//     original key, in the map's unspecified walk order), recursively;
//   - atoms `#<atom 1>`, futures `#<future 1>` / `#<future pending>`,
//     promises `#<promise 1>` / `#<promise pending>`, actors `#<actor 1>`
//     (the current state, printed in the same mode), user fns `#<fn>` and
//     built-in functions `#<fn NAME>` (`#<fn println>`);
//   - anything else as `#<unprintable>`.
// Runtime objects are recognised by the prototypes in the ActiveCallContext;
// without one only the literal kinds and collections render.
//
// Strings are plain protoCore strings, as the reader yields them and as
// programs build them, so their bytes are read directly.
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
        constexpr proto::proto_ulong kSmallIntMask  = PROTO_UL(0x3FF);
        constexpr proto::proto_ulong kSmallIntValue = PROTO_UL(0x001);
        proto::proto_ulong bits = reinterpret_cast<proto::proto_ulong>(v);
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
        proto::proto_ulong n = lst->getSize(ctx);
        for (proto::proto_ulong i = 0; i < n; ++i) {
            if (i > 0) out += ' ';
            printTo(ctx, out, lst->getAt(ctx, static_cast<int>(i)), readable);
        }
        out += ')';
        return;
    }
    if (isSeqObject(ctx, v)) {
        // A lazy seq, a cons or a chunk prints as a list, realised as it is
        // printed (an endless one never finishes, as in Clojure without
        // *print-length*). The walk's position is the only thing it holds.
        proto::ProtoContext scope(ctx->space, ctx);
        scope.resizeAutomaticLocals(1);
        SeqWalk w(&scope, 0, &scope, v);
        out += '(';
        for (bool firstEl = true; !w.done(); w.advance(&scope), firstEl = false) {
            if (!firstEl) out += ' ';
            printTo(&scope, out, w.first(&scope), readable);
        }
        out += ')';
        return;
    }
    if (isVector(v)) {
        const proto::ProtoList* items = vectorItems(ctx, v);
        out += '[';
        proto::proto_ulong n = items->getSize(ctx);
        for (proto::proto_ulong i = 0; i < n; ++i) {
            if (i > 0) out += ' ';
            printTo(ctx, out, items->getAt(ctx, static_cast<int>(i)), readable);
        }
        out += ']';
        return;
    }
    // A map, `{k v, k v}`: each entry's original key, in the map's
    // unspecified walk order (MapOps.h).
    if (isMap(v)) {
        out += '{';
        struct Acc { std::string* out; bool readable; bool first; }
            acc{&out, readable, true};
        mapForEach(ctx, v, &acc,
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
    if (proto::ProtoObject::isStringTagFast(v)) {
        const std::string bytes =
            reinterpret_cast<const proto::ProtoString*>(v)->toStdString(ctx);
        if (readable) appendReadableString(out, bytes);
        else          out += bytes;
        return;
    }
    // A pattern, `#"src"` readably and its source otherwise, as Clojure
    // prints a java.util.regex.Pattern (Regex.h).
    if (isPattern(ctx, v)) {
        if (readable) { out += "#\""; out += patternSource(ctx, v); out += '"'; }
        else          out += patternSource(ctx, v);
        return;
    }
    if (isMatcher(ctx, v)) { out += "#<matcher>"; return; }
    // A set, `#{a b}`, in its unspecified walk order (SetOps.h).
    if (isSet(ctx, v)) {
        out += "#{";
        struct Acc { std::string* out; bool readable; bool first; }
            acc{&out, readable, true};
        setForEach(ctx, v, &acc,
            [](proto::ProtoContext* c, void* self, const proto::ProtoObject* e) {
                auto* a = static_cast<Acc*>(self);
                if (!a->first) *a->out += ' ';
                a->first = false;
                printTo(c, *a->out, e, a->readable);
            });
        out += '}';
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
    // An exception: `#error {:type C, :message "m", ...}` (Exceptions.h).
    if (isException(ctx, v)) {
        appendExceptionForm(ctx, out, v);
        return;
    }
    // An I/O handle: `#<file a/b>`, `#<reader in.txt>`, ... (IO.h).
    if (appendIOHandle(ctx, out, v)) return;
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) { out += "#<unprintable>"; return; }
    if (isNamed(ctx, cc->named, v)) {
        out += namedSpelling(ctx, cc->named, v)->toStdString(ctx);
        return;
    }
    const proto::ProtoObject* prototype = v->getPrototype(ctx);
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
        bool delivered = false;
        const proto::ProtoObject* value =
            promiseValue(ctx, cc->valueKey, v, &delivered);
        if (delivered) {
            out += "#<promise ";
            printTo(ctx, out, value, readable);
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
    // A bare exception renders as its toString, as JVM Clojure's `str`
    // renders a Throwable ("ExceptionInfo: boom {:a 1}").
    if (isException(ctx, v)) {
        out += exceptionToString(ctx, v);
        return;
    }
    // A file renders as its path, as JVM Clojure's `str` renders a File.
    if (appendIOFilePath(ctx, out, v)) return;
    // A pattern renders as its source, as Pattern.toString does.
    if (isPattern(ctx, v)) { out += patternSource(ctx, v); return; }
    // A lazy seq renders as JVM Clojure's LazySeq.toString, the class name
    // and the hash of its elements (which realises it): `(str (map inc [1]))`
    // is "clojure.lang.LazySeq@..." there too. The hash digits differ from
    // the JVM's (D48).
    if (seqObjectKind(ctx, v) == SeqObject::Lazy) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%x",
                      static_cast<unsigned>(keyHash(ctx, v) & 0xFFFFFFFFu));
        out += "clojure.lang.LazySeq@";
        out += buf;
        return;
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
                                       const proto::ProtoArguments* args,
                                       const proto::ProtoKeywordArguments* /*kwargs*/) {
    proto::proto_ulong n = args ? args->getSize(ctx) : 0;
    std::string line;
    for (proto::proto_ulong i = 0; i < n; ++i) {
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
                                      const proto::ProtoArguments* args,
                                      proto::proto_ulong n, Op op) {
    if (n == 2) return op(args->getAt(ctx, 0), args->getAt(ctx, 1));
    proto::ProtoContext::CriticalSection guard(ctx);
    const proto::ProtoObject* acc = args->getAt(ctx, 0);
    for (proto::proto_ulong i = 1; i < n; ++i)
        acc = op(acc, args->getAt(ctx, static_cast<int>(i)));
    return acc;
}

void checkNumbers(proto::ProtoContext* ctx, const proto::ProtoArguments* args,
                  proto::proto_ulong n, const char* primName) {
    for (proto::proto_ulong i = 0; i < n; ++i)
        numberArg(ctx, args, static_cast<int>(i), primName);
}

const proto::ProtoObject* prim_plus(proto::ProtoContext* ctx,
                                    const proto::ProtoObject*,
                                    const proto::ParentLink*,
                                    const proto::ProtoArguments* args,
                                    const proto::ProtoKeywordArguments*) {
    proto::proto_ulong n = args ? args->getSize(ctx) : 0;
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
                                     const proto::ProtoArguments* args,
                                     const proto::ProtoKeywordArguments*) {
    proto::proto_ulong n = args ? args->getSize(ctx) : 0;
    if (n == 0) throwError(ctx, "- : needs at least one arg");
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
                                   const proto::ProtoArguments* args,
                                   const proto::ProtoKeywordArguments*) {
    proto::proto_ulong n = args ? args->getSize(ctx) : 0;
    checkNumbers(ctx, args, n, "*");
    if (n == 0) return ctx->fromLong(1);
    if (n == 1) return args->getAt(ctx, 0);
    return numericFold(ctx, args, n,
        [ctx](const proto::ProtoObject* a, const proto::ProtoObject* b) {
            return a->multiply(ctx, b);
        });
}

// `/` — when every argument is an integer, the quotient truncates toward
// zero (there are no ratios yet) and stays exact at any magnitude, and a zero
// divisor raises the ArithmeticException analogue with JVM Clojure's message,
// "Divide by zero". Once any argument is a float, every argument is taken as
// a double and the division follows IEEE 754, as in JVM Clojure: a zero
// divisor gives ##Inf, ##-Inf or ##NaN.
const proto::ProtoObject* prim_div(proto::ProtoContext* ctx,
                                   const proto::ProtoObject*,
                                   const proto::ParentLink*,
                                   const proto::ProtoArguments* args,
                                   const proto::ProtoKeywordArguments*) {
    proto::proto_ulong n = args ? args->getSize(ctx) : 0;
    if (n == 0) throwError(ctx, "/: needs at least one arg");
    checkNumbers(ctx, args, n, "/");
    bool anyFloat = false;
    for (proto::proto_ulong i = 0; i < n && !anyFloat; ++i)
        anyFloat = args->getAt(ctx, static_cast<int>(i))->isFloat(ctx);
    if (anyFloat) {
        if (n == 1) return ctx->fromDouble(1.0 / args->getAt(ctx, 0)->asDouble(ctx));
        double acc = args->getAt(ctx, 0)->asDouble(ctx);
        for (proto::proto_ulong i = 1; i < n; ++i)
            acc /= args->getAt(ctx, static_cast<int>(i))->asDouble(ctx);
        return ctx->fromDouble(acc);
    }
    // A zero divisor: protoCore raises its DivisionByZero error, which the
    // error factory (Exceptions.cpp) builds as ArithmeticException "Divide
    // by zero".
    if (n == 1) return ctx->fromLong(1)->divide(ctx, args->getAt(ctx, 0));
    return numericFold(ctx, args, n,
        [ctx](const proto::ProtoObject* a, const proto::ProtoObject* b) {
            return a->divide(ctx, b);
        });
}

const proto::ProtoObject* prim_inc(proto::ProtoContext* ctx,
                                   const proto::ProtoObject*,
                                   const proto::ParentLink*,
                                   const proto::ProtoArguments* args,
                                   const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1)
        throwError(ctx, "inc: expects 1 arg");
    const proto::ProtoObject* x = numberArg(ctx, args, 0, "inc");
    if (x->isFloat(ctx)) return ctx->fromDouble(x->asDouble(ctx) + 1.0);
    return x->add(ctx, ctx->fromLong(1));
}

const proto::ProtoObject* prim_dec(proto::ProtoContext* ctx,
                                   const proto::ProtoObject*,
                                   const proto::ParentLink*,
                                   const proto::ProtoArguments* args,
                                   const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1)
        throwError(ctx, "dec: expects 1 arg");
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
                                         const proto::ProtoArguments* args,
                                         const char* name,
                                         DoublePred holdsForDoubles,
                                         SignPred holdsForSign) {
    proto::proto_ulong n = args ? args->getSize(ctx) : 0;
    if (n < 2) return PROTO_TRUE;            // 0- or 1-arg form is true
    const proto::ProtoObject* prev = numberArg(ctx, args, 0, name);
    for (proto::proto_ulong i = 1; i < n; ++i) {
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
                                  const proto::ProtoArguments* args,
                                  const proto::ProtoKeywordArguments*) {
    return monotonicChain(ctx, args, "<", [](double a, double b){ return a < b; },
                          [](int sign){ return sign < 0; });
}

const proto::ProtoObject* prim_le(proto::ProtoContext* ctx,
                                  const proto::ProtoObject*,
                                  const proto::ParentLink*,
                                  const proto::ProtoArguments* args,
                                  const proto::ProtoKeywordArguments*) {
    return monotonicChain(ctx, args, "<=", [](double a, double b){ return a <= b; },
                          [](int sign){ return sign <= 0; });
}

const proto::ProtoObject* prim_gt(proto::ProtoContext* ctx,
                                  const proto::ProtoObject*,
                                  const proto::ParentLink*,
                                  const proto::ProtoArguments* args,
                                  const proto::ProtoKeywordArguments*) {
    return monotonicChain(ctx, args, ">", [](double a, double b){ return a > b; },
                          [](int sign){ return sign > 0; });
}

const proto::ProtoObject* prim_ge(proto::ProtoContext* ctx,
                                  const proto::ProtoObject*,
                                  const proto::ParentLink*,
                                  const proto::ProtoArguments* args,
                                  const proto::ProtoKeywordArguments*) {
    return monotonicChain(ctx, args, ">=", [](double a, double b){ return a >= b; },
                          [](int sign){ return sign >= 0; });
}

const proto::ProtoObject* prim_eq(proto::ProtoContext* ctx,
                                  const proto::ProtoObject*,
                                  const proto::ParentLink*,
                                  const proto::ProtoArguments* args,
                                  const proto::ProtoKeywordArguments*) {
    // Value equality (valuesEqual) over every adjacent pair, like Clojure's
    // variadic =. The 0- and 1-argument forms are true.
    proto::proto_ulong n = args ? args->getSize(ctx) : 0;
    if (n < 2) return PROTO_TRUE;
    for (proto::proto_ulong i = 1; i < n; ++i) {
        if (!valuesEqual(ctx, args->getAt(ctx, static_cast<int>(i - 1)),
                         args->getAt(ctx, static_cast<int>(i))))
            return PROTO_FALSE;
    }
    return PROTO_TRUE;
}

const proto::ProtoObject* prim_not_eq(proto::ProtoContext* ctx,
                                      const proto::ProtoObject* self,
                                      const proto::ParentLink* parentLink,
                                      const proto::ProtoArguments* args,
                                      const proto::ProtoKeywordArguments* kwArgs) {
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
                                   const proto::ProtoArguments* args,
                                   const proto::ProtoKeywordArguments*) {
    proto::proto_ulong n = args ? args->getSize(ctx) : 0;
    std::string text;
    for (proto::proto_ulong i = 0; i < n; ++i) {
        appendStr(ctx, text, args->getAt(ctx, (int)i));
    }
    return makeUtf8String(ctx, text);
}

// List + predicate primitives -----------------------------------

// Treat nil as the empty seq. A vector's elements already ARE a ProtoList
// (VectorOps.h), so seq-coercing one is an unbox, not a conversion. Strings
// stay opaque for the first version; later changes added string support.
bool pushSeqElements(ListBuilder& out, const proto::ProtoObject* v);

// A list or vector is read in place; a map (its entries as [k v] vectors),
// a set (its elements) and a string (its characters) are walked into a new
// list, as `seq` builds it; anything else raises Clojure's "Don't know how
// to create ISeq from" IllegalArgumentException.
const proto::ProtoList* asSeqOrNull(proto::ProtoContext* ctx,
                                    const proto::ProtoObject* v) {
    if (!v || v == PROTO_NONE) return nullptr;
    if (isListTag(v)) return v->asList(ctx);
    if (isVector(v)) return vectorItems(ctx, v);
    // A lazy seq, a cons or a chunk: realised to its end (LazySeq.h). The
    // functions that need only a prefix (first, next, nth, take, ...) and
    // those that walk without keeping the elements (count, reduce, dorun)
    // read it through the seq protocol instead.
    if (isSeqObject(ctx, v)) {
        const proto::ProtoList* lst = seqToList(ctx, v);
        return lst ? lst : ctx->newList();
    }
    ListBuilder out(ctx);
    if (!pushSeqElements(out, v))
        throwClassed(ctx, "IllegalArgumentException",
                     "Don't know how to create ISeq from: " + std::string(valueTypeName(ctx, v)));
    return out.finishList();
}

const proto::ProtoObject* prim_list(proto::ProtoContext* ctx,
                                    const proto::ProtoObject*,
                                    const proto::ParentLink*,
                                    const proto::ProtoArguments* args,
                                    const proto::ProtoKeywordArguments*) {
    // The arguments as a ProtoList, built once in bulk (asList); the call's
    // own list when it came with one. Copying element by element rebuilt
    // the whole tree and left one root-to-leaf path of garbage per element.
    if (!args) return ctx->newList()->asObject(ctx);
    return args->asList(ctx)->asObject(ctx);
}

// `(vector x y z)`, and the bytecode form `[x y z]`, which desugars to a
// call here. A vector's elements ARE a ProtoList (VectorOps.h), so building
// one is the arguments' list (asList, built once in bulk) in a box.
const proto::ProtoObject* prim_vector(proto::ProtoContext* ctx,
                                      const proto::ProtoObject*,
                                      const proto::ParentLink*,
                                      const proto::ProtoArguments* args,
                                      const proto::ProtoKeywordArguments*) {
    return newVector(ctx, args ? args->asList(ctx) : ctx->newList());
}

// (vec coll) — converts any seqable (list or vector) to a vector.
const proto::ProtoObject* prim_vec(proto::ProtoContext* ctx,
                                   const proto::ProtoObject*,
                                   const proto::ParentLink*,
                                   const proto::ProtoArguments* args,
                                   const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1)
        throwError(ctx, "vec: expects 1 arg");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    if (isVector(v)) return v;  // already a vector
    const proto::ProtoList* lst = asSeqOrNull(ctx, v);
    return newVector(ctx, lst ? lst : ctx->newList());
}

// `v` as println prints it, for error messages (an index argument keeps all
// its digits, at any magnitude).
std::string printedValue(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
    std::string text;
    printTo(ctx, text, v, /*readable=*/true);
    return text;
}

// The IndexOutOfBoundsException analogue of nth: the index as written and
// the collection's count.
[[noreturn, gnu::cold]]
void throwNthOutOfBounds(proto::ProtoContext* ctx, const proto::ProtoObject* index,
                         long long count) {
    throwClassed(ctx, "IndexOutOfBoundsException", "nth index " +
                             printedValue(ctx, index) + " is out of bounds (count " +
                             std::to_string(count) + ")");
}

// (nth coll i) / (nth coll i not-found), as JVM Clojure's nth:
//   - a vector in O(log N), a list in O(N);
//   - a string: the character at code-point index `i`, as a one-character
//     string (deviation D3), indexed like count and subs;
//   - nil: nil, or not-found, for any integer index;
//   - an index out of range raises IndexOutOfBoundsException
//     (StringIndexOutOfBoundsException for a string), or returns not-found;
//   - any other value raises UnsupportedOperationException.
const proto::ProtoObject* prim_nth(proto::ProtoContext* ctx,
                                   const proto::ProtoObject*,
                                   const proto::ParentLink*,
                                   const proto::ProtoArguments* args,
                                   const proto::ProtoKeywordArguments*) {
    proto::proto_ulong ac = args ? args->getSize(ctx) : 0;
    if (ac != 2 && ac != 3)
        throwError(ctx, "nth: expects (nth coll i) or (nth coll i nf)");
    const proto::ProtoObject* coll = args->getAt(ctx, 0);
    long long idx = indexArg(ctx, args, 1, "nth");
    const proto::ProtoObject* notFound = (ac == 3) ? args->getAt(ctx, 2) : nullptr;

    if (!coll || coll == PROTO_NONE) return notFound ? notFound : PROTO_NONE;

    // String path: a one-character string (a slice), O(log N).
    if (proto::ProtoObject::isStringTagFast(coll)) {
        const auto* s = reinterpret_cast<const proto::ProtoString*>(coll);
        const long long sz = static_cast<long long>(s->getSize(ctx));
        if (idx < 0 || idx >= sz) {
            if (notFound) return notFound;
            throwClassed(ctx, "StringIndexOutOfBoundsException", "nth index " +
                                     printedValue(ctx, args->getAt(ctx, 1)) +
                                     " is out of bounds (count " +
                                     std::to_string(sz) + ")");
        }
        return reinterpret_cast<const proto::ProtoObject*>(
            s->getSlice(ctx, static_cast<int>(idx), static_cast<int>(idx) + 1));
    }

    const bool vector = isVector(coll);
    if (isSeqObject(ctx, coll)) {
        // A lazy seq, a cons or a chunk: walked to the index, O(i).
        if (idx < 0) {
            if (notFound) return notFound;
            throwNthOutOfBounds(ctx, args->getAt(ctx, 1), 0);
        }
        proto::ProtoContext scope(ctx->space, ctx);
        scope.resizeAutomaticLocals(1);
        SeqWalk w(&scope, 0, &scope, coll);
        long long i = 0;
        for (; i < idx && !w.done(); ++i) w.advance(&scope);
        if (w.done()) {
            if (notFound) return notFound;
            throwNthOutOfBounds(ctx, args->getAt(ctx, 1), i);
        }
        return w.first(&scope);
    }
    if (!vector && !isListTag(coll)) {
        throwClassed(ctx, "UnsupportedOperationException",
            std::string("nth not supported on ") +
            valueTypeName(ctx, coll));
    }

    // Both paths are one indexed read of a ProtoList, O(log N); a vector
    // costs one unbox on top.
    const proto::ProtoList* lst = vector ? vectorItems(ctx, coll) : coll->asList(ctx);
    long long sz = static_cast<long long>(lst->getSize(ctx));
    if (idx < 0 || idx >= sz) {
        if (notFound) return notFound;
        throwNthOutOfBounds(ctx, args->getAt(ctx, 1), sz);
    }
    return lst->getAt(ctx, static_cast<int>(idx));
}

const proto::ProtoObject* prim_vector_p(proto::ProtoContext* ctx,
                                        const proto::ProtoObject*,
                                        const proto::ParentLink*,
                                        const proto::ProtoArguments* args,
                                        const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1)
        throwError(ctx, "vector?: expects 1 arg");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    return isVector(v) ? PROTO_TRUE : PROTO_FALSE;
}

const proto::ProtoObject* prim_list_p(proto::ProtoContext* ctx,
                                      const proto::ProtoObject*,
                                      const proto::ParentLink*,
                                      const proto::ProtoArguments* args,
                                      const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1)
        throwError(ctx, "list?: expects 1 arg");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    return (v && isListTag(v)) ? PROTO_TRUE : PROTO_FALSE;
}

// Session 13 — map primitives. The representation (a ProtoMap driven
// through protoCore's hashed-collection helper), the key semantics, the cost
// model and the GC-rooting rules live in src/runtime/MapOps.h; the
// primitives below only validate arguments and delegate. Map equality under
// `=` is mapEquals, reached through valuesEqual.

const proto::ProtoObject* prim_map_p(proto::ProtoContext* ctx,
                                     const proto::ProtoObject*,
                                     const proto::ParentLink*,
                                     const proto::ProtoArguments* args,
                                     const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1)
        throwError(ctx, "map?: expects 1 arg");
    return isMap(args->getAt(ctx, 0)) ? PROTO_TRUE : PROTO_FALSE;
}

// Also the target of every `{...}` literal, whose entries the compiler
// passes in source order.
const proto::ProtoObject* prim_hash_map(proto::ProtoContext* ctx,
                                        const proto::ProtoObject*,
                                        const proto::ParentLink*,
                                        const proto::ProtoArguments* args,
                                        const proto::ProtoKeywordArguments*) {
    proto::proto_ulong n = args ? args->getSize(ctx) : 0;
    if (n % 2 != 0)
        throwError(ctx, "hash-map: needs an even number of args");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throwError(ctx, "hash-map: no active VM context");
    return mapAssocPairs(ctx, nullptr, args->values(), n);
}

const proto::ProtoObject* prim_assoc(proto::ProtoContext* ctx,
                                     const proto::ProtoObject*,
                                     const proto::ParentLink*,
                                     const proto::ProtoArguments* args,
                                     const proto::ProtoKeywordArguments*) {
    proto::proto_ulong n = args ? args->getSize(ctx) : 0;
    if (n < 3 || (n - 1) % 2 != 0)
        throwError(ctx, "assoc: expects (assoc m k v ...)");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throwError(ctx, "assoc: no active VM context");
    const proto::ProtoObject* m = args->getAt(ctx, 0);
    // nil is the empty map, as in Clojure: (assoc nil :a 1) is {:a 1}, which
    // assoc-in and update-in rely on.
    if (!m || m == PROTO_NONE) return mapAssocPairs(ctx, nullptr, args->values() + 1, n - 1);
    if (isVector(m)) {
        // A vector by index, 0 to its count (the count appends), as in
        // Clojure; any other index raises IndexOutOfBoundsException.
        proto::ProtoContext scope(ctx->space, ctx);
        scope.resizeAutomaticLocals(1);
        scope.setAutomaticLocal(0, vectorItems(&scope, m)->asObject(&scope));
        for (proto::proto_ulong i = 1; i + 1 < n; i += 2) {
            const proto::ProtoList* items = scope.getAutomaticLocal(0)->asList(&scope);
            const proto::ProtoObject* k = args->getAt(&scope, static_cast<int>(i));
            const long long size = static_cast<long long>(items->getSize(&scope));
            if (!proto::isSmallInt(k) || proto::asSmallInt(k) < 0 || proto::asSmallInt(k) > size)
                throwClassed(&scope, "IndexOutOfBoundsException",
                             "assoc index " + printedValue(&scope, k) + " is out of bounds (count " +
                             std::to_string(size) + ")");
            const long long idx = proto::asSmallInt(k);
            const proto::ProtoObject* v = args->getAt(&scope, static_cast<int>(i + 1));
            items = idx == size ? items->appendLast(&scope, v)
                                : items->setAt(&scope, static_cast<int>(idx), v);
            scope.setAutomaticLocal(0, items->asObject(&scope));
        }
        const proto::ProtoObject* r = newVector(&scope, scope.getAutomaticLocal(0)->asList(&scope));
        scope.returnValue = r;
        return r;
    }
    if (!isMap(m))
        throwClassed(ctx, "ClassCastException",
                     std::string("assoc expects a map, a vector or nil, got ") + valueTypeName(ctx, m));
    return mapAssocPairs(ctx, m, args->values() + 1, n - 1);
}

// (dissoc m) / (dissoc m k & ks) — `m` without the given keys, matched by
// value as `get` matches them. An absent key leaves the map unchanged,
// (dissoc m) is `m` and (dissoc nil k ...) is nil, as in Clojure; any other
// first argument raises the ClassCastException analogue.
const proto::ProtoObject* prim_dissoc(proto::ProtoContext* ctx,
                                      const proto::ProtoObject*,
                                      const proto::ParentLink*,
                                      const proto::ProtoArguments* args,
                                      const proto::ProtoKeywordArguments*) {
    proto::proto_ulong n = args ? args->getSize(ctx) : 0;
    if (n < 1)
        throwError(ctx, "dissoc: expects (dissoc m k ...)");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throwError(ctx, "dissoc: no active VM context");
    const proto::ProtoObject* m = args->getAt(ctx, 0);
    if (!m || m == PROTO_NONE) return PROTO_NONE;
    if (!isMap(m)) {
        throwClassed(ctx, "ClassCastException", std::string("dissoc expects a map, got ") +
                                 valueTypeName(ctx, m));
    }
    // mapDissoc returns an unrooted map: each intermediate map is rooted in
    // the scope slot before the next key is removed.
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(1);
    scope.setAutomaticLocal(0, m);
    for (proto::proto_ulong i = 1; i < n; ++i) {
        scope.setAutomaticLocal(0,
            mapDissoc(&scope, scope.getAutomaticLocal(0),
                      args->getAt(&scope, static_cast<int>(i))));
    }
    return scope.getAutomaticLocal(0);
}

const proto::ProtoObject* prim_get(proto::ProtoContext* ctx,
                                   const proto::ProtoObject*,
                                   const proto::ParentLink*,
                                   const proto::ProtoArguments* args,
                                   const proto::ProtoKeywordArguments*) {
    proto::proto_ulong n = args ? args->getSize(ctx) : 0;
    if (n != 2 && n != 3)
        throwError(ctx, "get: expects (get m k) or (get m k not-found)");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throwError(ctx, "get: no active VM context");
    const proto::ProtoObject* m = args->getAt(ctx, 0);
    const proto::ProtoObject* k = args->getAt(ctx, 1);
    const proto::ProtoObject* nf = (n == 3) ? args->getAt(ctx, 2) : PROTO_NONE;
    bool found = false;
    // A map first: the common case pays one tag test.
    if (isMap(m)) {
        const proto::ProtoObject* v = mapGet(ctx, m, k, &found);
        return found ? v : nf;
    }
    if (isSet(ctx, m)) {
        const proto::ProtoObject* e = setGet(ctx, m, k, &found);
        return found ? e : nf;
    }
    return nf;
}

const proto::ProtoObject* prim_contains_p(proto::ProtoContext* ctx,
                                          const proto::ProtoObject*,
                                          const proto::ParentLink*,
                                          const proto::ProtoArguments* args,
                                          const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 2)
        throwError(ctx, "contains?: expects (contains? coll k)");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throwError(ctx, "contains?: no active VM context");
    const proto::ProtoObject* m = args->getAt(ctx, 0);
    const proto::ProtoObject* k = args->getAt(ctx, 1);
    bool found = false;
    if (isMap(m)) {
        mapGet(ctx, m, k, &found);
        return found ? PROTO_TRUE : PROTO_FALSE;
    }
    if (isSet(ctx, m)) {
        setGet(ctx, m, k, &found);
        return found ? PROTO_TRUE : PROTO_FALSE;
    }
    return PROTO_FALSE;
}

// Return a fresh ProtoList of the original keys or of the values of `m`, in
// the map's unspecified walk order, which is the same for keys and vals of
// one map value. The list under construction lives in an automatic local
// (P1).
static const proto::ProtoObject* mapWalk(proto::ProtoContext* ctx,
                                         const proto::ProtoObject* m,
                                         bool wantValues) {
    if (!isMap(m))
        return ctx->newList()->asObject(ctx);
    // One entry per map entry, so the result grows with user data: chunked.
    ListBuilder out(ctx);
    struct Acc { ListBuilder* out; bool wantValues; }
        acc{&out, wantValues};
    mapForEach(out.context(), m, &acc,
        [](proto::ProtoContext*, void* self,
           const proto::ProtoObject* k, const proto::ProtoObject* v) {
            auto* a = static_cast<Acc*>(self);
            a->out->push(a->wantValues ? v : k);
        });
    return out.finish();
}

const proto::ProtoObject* prim_keys(proto::ProtoContext* ctx,
                                    const proto::ProtoObject*,
                                    const proto::ParentLink*,
                                    const proto::ProtoArguments* args,
                                    const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1)
        throwError(ctx, "keys: expects 1 arg");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throwError(ctx, "keys: no active VM context");
    return mapWalk(ctx, args->getAt(ctx, 0), /*wantValues=*/false);
}

const proto::ProtoObject* prim_vals(proto::ProtoContext* ctx,
                                    const proto::ProtoObject*,
                                    const proto::ParentLink*,
                                    const proto::ProtoArguments* args,
                                    const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1)
        throwError(ctx, "vals: expects 1 arg");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throwError(ctx, "vals: no active VM context");
    return mapWalk(ctx, args->getAt(ctx, 0), /*wantValues=*/true);
}

const proto::ProtoObject* prim_first(proto::ProtoContext* ctx,
                                     const proto::ProtoObject*,
                                     const proto::ParentLink*,
                                     const proto::ProtoArguments* args,
                                     const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1)
        throwError(ctx, "first: expects 1 arg");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    const proto::ProtoObject* r;
    if (tryFirst(ctx, v, &r)) return r;
    const proto::ProtoObject* s = seqOf(ctx, v);
    return s == PROTO_NONE ? PROTO_NONE : seqFirst(ctx, s);
}

const proto::ProtoObject* prim_rest(proto::ProtoContext* ctx,
                                    const proto::ProtoObject*,
                                    const proto::ParentLink*,
                                    const proto::ProtoArguments* args,
                                    const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1)
        throwError(ctx, "rest: expects 1 arg");
    // Clojure's rest: what follows the first element, `()` when nothing
    // does (seqMore); a vector's is a chunked seq, as Clojure's ChunkedSeq.
    const proto::ProtoObject* r;
    if (tryRest(ctx, args->getAt(ctx, 0), &r)) return r;
    const proto::ProtoObject* s = seqOf(ctx, args->getAt(ctx, 0));
    if (s == PROTO_NONE) return ctx->newList()->asObject(ctx);
    return seqMore(ctx, s);
}

const proto::ProtoObject* prim_cons(proto::ProtoContext* ctx,
                                    const proto::ProtoObject*,
                                    const proto::ParentLink*,
                                    const proto::ProtoArguments* args,
                                    const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 2)
        throwError(ctx, "cons: expects (cons x coll)");
    const proto::ProtoObject* x    = args->getAt(ctx, 0);
    const proto::ProtoObject* coll = args->getAt(ctx, 1);
    // Onto a lazy seq, a cons or a chunk: a cons cell, which realises
    // nothing, as in Clojure. Onto a list (or any other collection, seq'd
    // into one): the list one longer.
    if (isSeqObject(ctx, coll)) return newCons(ctx, x, coll);
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
                                     const proto::ProtoArguments* args,
                                     const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1)
        throwError(ctx, "count: expects 1 arg");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    if (!v || v == PROTO_NONE) return ctx->fromLong(0);
    if (isStringLike(v)) {
        return ctx->fromLong(static_cast<long long>(asProtoString(v)->getSize(ctx)));
    }
    // A map: its number of entries; a set: its number of elements (O(n), D25).
    if (isMap(v)) return ctx->fromLong(static_cast<long long>(mapCount(ctx, v)));
    if (isSet(ctx, v)) return ctx->fromLong(static_cast<long long>(setCount(ctx, v)));
    if (isSeqObject(ctx, v)) {
        // Walked, not built: the count of a lazy seq nobody else holds
        // keeps none of its elements (releaseArg, SeqWalk).
        proto::ProtoContext scope(ctx->space, ctx);
        scope.resizeAutomaticLocals(1);
        SeqWalk w(&scope, 0, &scope, v);
        releaseArg(args, 0);
        long long n = 0;
        for (; !w.done(); w.advance(&scope)) ++n;
        return ctx->fromLong(n);
    }
    const proto::ProtoList* lst = asSeqOrNull(ctx, v);
    return ctx->fromLong(lst ? static_cast<long long>(lst->getSize(ctx)) : 0);
}

const proto::ProtoObject* prim_empty_p(proto::ProtoContext* ctx,
                                       const proto::ProtoObject*,
                                       const proto::ParentLink*,
                                       const proto::ProtoArguments* args,
                                       const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1)
        throwError(ctx, "empty?: expects 1 arg");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    bool e;
    if (tryEmpty(ctx, v, &e)) return e ? PROTO_TRUE : PROTO_FALSE;
    if (isStringLike(v)) {
        return asProtoString(v)->getSize(ctx) == 0 ? PROTO_TRUE : PROTO_FALSE;
    }
    if (isMap(v)) return mapIsEmpty(ctx, v) ? PROTO_TRUE : PROTO_FALSE;
    if (isSet(ctx, v)) return setIsEmpty(ctx, v) ? PROTO_TRUE : PROTO_FALSE;
    if (isSeqObject(ctx, v)) return seqOf(ctx, v) == PROTO_NONE ? PROTO_TRUE : PROTO_FALSE;
    const proto::ProtoList* lst = asSeqOrNull(ctx, v);
    return (!lst || lst->getSize(ctx) == 0) ? PROTO_TRUE : PROTO_FALSE;
}

const proto::ProtoObject* prim_nil_p(proto::ProtoContext* ctx,
                                     const proto::ProtoObject*,
                                     const proto::ParentLink*,
                                     const proto::ProtoArguments* args,
                                     const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1)
        throwError(ctx, "nil?: expects 1 arg");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    return (!v || v == PROTO_NONE) ? PROTO_TRUE : PROTO_FALSE;
}

const proto::ProtoObject* prim_not(proto::ProtoContext* ctx,
                                   const proto::ProtoObject*,
                                   const proto::ParentLink*,
                                   const proto::ProtoArguments* args,
                                   const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1)
        throwError(ctx, "not: expects 1 arg");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    return (!v || v == PROTO_NONE || v == PROTO_FALSE) ? PROTO_TRUE : PROTO_FALSE;
}

const proto::ProtoObject* prim_reverse(proto::ProtoContext* ctx,
                                       const proto::ProtoObject*,
                                       const proto::ParentLink*,
                                       const proto::ProtoArguments* args,
                                       const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1)
        throwError(ctx, "reverse: expects 1 arg");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    // String input → reverse character by character (codepoint-aware
    // via std::string + UTF-8 walk would be a follow-up; v0.15 reverses
    // raw bytes which is correct for ASCII).
    if (isStringLike(v)) {
        const proto::ProtoString* s = asProtoString(v);
        std::string raw = s->toStdString(ctx);
        std::string r(raw.rbegin(), raw.rend());
        return reinterpret_cast<const proto::ProtoObject*>(
            makeUtf8ProtoString(ctx, r));
    }
    const proto::ProtoList* lst = asSeqOrNull(ctx, v);
    if (!lst) return ctx->newList()->asObject(ctx);
    // Walk the source backwards and append at the END, which lets the build
    // run through ListBuilder; appending at the front produces the same list
    // but has no chunked form.
    ListBuilder out(ctx);
    for (proto::proto_ulong i = lst->getSize(out.context()); i-- > 0;) {
        out.push(lst->getAt(out.context(), static_cast<int>(i)));
    }
    return out.finish();
}

// The elements of `v` as a seq builds them, pushed onto `out`: a list's or
// vector's elements, a map's entries as [key value] vectors, a string's
// characters as one-character strings (D3). nil has none. Returns false,
// pushing nothing, for any other value. Every element is reachable from `v`,
// which the caller roots; the entry vectors are parked by `push`.
bool pushSeqElements(ListBuilder& out, const proto::ProtoObject* v) {
    proto::ProtoContext* c = out.context();
    if (!v || v == PROTO_NONE) return true;
    if (isSeqObject(c, v)) {
        // Realised to its end in contexts of its own (seqToList), then
        // pushed; the list stays anchored in the builder's context.
        if (const proto::ProtoList* lst = seqToList(c, v)) {
            const proto::proto_ulong n = lst->getSize(c);
            for (proto::proto_ulong i = 0; i < n; ++i)
                out.push(lst->getAt(out.context(), static_cast<int>(i)));
        }
        return true;
    }
    if (isListTag(v) || isVector(v)) {
        const proto::ProtoList* lst = isVector(v) ? vectorItems(c, v) : v->asList(c);
        const proto::proto_ulong n = lst->getSize(c);
        for (proto::proto_ulong i = 0; i < n; ++i)
            out.push(lst->getAt(out.context(), static_cast<int>(i)));
        return true;
    }
    if (isMap(v)) {
        std::vector<const proto::ProtoObject*> kv;
        mapForEach(c, v, &kv,
            [](proto::ProtoContext*, void* self, const proto::ProtoObject* k,
               const proto::ProtoObject* val) {
                auto* acc = static_cast<std::vector<const proto::ProtoObject*>*>(self);
                acc->push_back(k);
                acc->push_back(val);
            });
        for (std::size_t i = 0; i < kv.size(); i += 2) {
            proto::ProtoContext* ec = out.context();
            out.push(newVector(ec, ec->newList()->appendLast(ec, kv[i])->appendLast(ec, kv[i + 1])));
        }
        return true;
    }
    if (isSet(c, v)) {
        std::vector<const proto::ProtoObject*> items;
        setForEach(c, v, &items,
            [](proto::ProtoContext*, void* self, const proto::ProtoObject* e) {
                static_cast<std::vector<const proto::ProtoObject*>*>(self)->push_back(e);
            });
        for (const proto::ProtoObject* e : items) out.push(e);
        return true;
    }
    if (isStringLike(v)) {
        const std::string raw = asProtoString(v)->toStdString(c);
        for (std::size_t i = 0; i < raw.size();) {
            const unsigned char lead = static_cast<unsigned char>(raw[i]);
            const std::size_t len = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2
                                  : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 1;
            out.push(makeUtf8String(out.context(), raw.substr(i, len)));
            i += len;
        }
        return true;
    }
    return false;
}

// (seq coll) — the elements of a list, vector, map or string as a list, or
// nil when there are none (pushSeqElements). Clojure answers a seq view;
// here it is a list, which prints and walks the same.
const proto::ProtoObject* prim_seq(proto::ProtoContext* ctx,
                                   const proto::ProtoObject*,
                                   const proto::ParentLink*,
                                   const proto::ProtoArguments* args,
                                   const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1)
        throwError(ctx, "seq: expects 1 arg");
    // RT.seq (LazySeq.h): a list itself, a vector's chunked seq, a lazy seq's
    // realised seq, a map's / set's / string's list.
    return seqOf(ctx, args->getAt(ctx, 0));
}

// (concat coll ...) — one list of the elements of every argument in order
// (each a list, vector, map, string or nil). Clojure answers a lazy seq;
// here it is a list, built eagerly (D29's reason: no lazy seqs yet).
const proto::ProtoObject* prim_concat(proto::ProtoContext* ctx,
                                      const proto::ProtoObject*,
                                      const proto::ParentLink*,
                                      const proto::ProtoArguments* args,
                                      const proto::ProtoKeywordArguments*) {
    const proto::proto_ulong n = args ? args->getSize(ctx) : 0;
    // With a lazy seq among the arguments, a lazy concatenation (LazySeq.h),
    // which realises nothing now. When every argument is an already realised
    // collection there is nothing laziness could defer, so the list is built
    // at once: the same elements in the same order, and syntax-quote's
    // expansions, which call concat on lists, stay as cheap as before.
    for (proto::proto_ulong i = 0; i < n; ++i) {
        if (isSeqObject(ctx, args->getAt(ctx, static_cast<int>(i)))) {
            proto::ProtoContext scope(ctx->space, ctx);
            scope.resizeAutomaticLocals(1);
            scope.setAutomaticLocal(0, args->asList(&scope)->asObject(&scope));
            const proto::ProtoObject* r =
                lazyStep(&scope, StepKind::Concat, PROTO_NONE, scope.getAutomaticLocal(0));
            scope.returnValue = r;
            return r;
        }
    }
    ListBuilder out(ctx);
    for (proto::proto_ulong i = 0; i < n; ++i) {
        const proto::ProtoObject* v = args->getAt(out.context(), static_cast<int>(i));
        if (!pushSeqElements(out, v))
            throwError(out.context(), "Don't know how to create ISeq from: " +
                                      printedValue(out.context(), v));
    }
    return out.finish();
}

// (symbol? x) / (keyword? x) — named values (Named.h) told apart by the
// keyword's leading `:`.
bool namedStartsWithColon(proto::ProtoContext* ctx, const ActiveCallContext* cc,
                          const proto::ProtoObject* v, bool* isNamedValue) {
    *isNamedValue = cc && isNamed(ctx, cc->named, v);
    if (!*isNamedValue) return false;
    const std::string s = namedSpelling(ctx, cc->named, v)->toStdString(ctx);
    return !s.empty() && s[0] == ':';
}

const proto::ProtoObject* prim_symbol_p(proto::ProtoContext* ctx,
                                        const proto::ProtoObject*,
                                        const proto::ParentLink*,
                                        const proto::ProtoArguments* args,
                                        const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1)
        throwError(ctx, "symbol?: expects 1 arg");
    bool named = false;
    const bool colon = namedStartsWithColon(ctx, activeCallContext(), args->getAt(ctx, 0), &named);
    return (named && !colon) ? PROTO_TRUE : PROTO_FALSE;
}

const proto::ProtoObject* prim_keyword_p(proto::ProtoContext* ctx,
                                         const proto::ProtoObject*,
                                         const proto::ParentLink*,
                                         const proto::ProtoArguments* args,
                                         const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1)
        throwError(ctx, "keyword?: expects 1 arg");
    bool named = false;
    const bool colon = namedStartsWithColon(ctx, activeCallContext(), args->getAt(ctx, 0), &named);
    return (named && colon) ? PROTO_TRUE : PROTO_FALSE;
}

// Higher-order primitives --------------------------------------

const proto::ProtoObject* prim_reduce(proto::ProtoContext* ctx,
                                      const proto::ProtoObject*,
                                      const proto::ParentLink*,
                                      const proto::ProtoArguments* args,
                                      const proto::ProtoKeywordArguments*) {
    // Two shapes: (reduce f coll) — init is first elem;
    //             (reduce f init coll) — explicit init.
    proto::proto_ulong ac = args ? args->getSize(ctx) : 0;
    if (ac != 2 && ac != 3)
        throwError(ctx, "reduce: expects (reduce f coll) or (reduce f init coll)");
    const proto::ProtoObject* f = args->getAt(ctx, 0);
    const proto::ProtoObject* coll = args->getAt(ctx, ac == 2 ? 1 : 2);
    if (isSeqObject(ctx, coll)) {
        // Walked: the head is released (releaseArg), so reducing a lazy seq
        // nobody else holds runs in constant memory; the accumulator lives
        // in a slot and the walk's garbage is handed over every 64 steps.
        const ActiveCallContext* cc = activeCallContext();
        if (!cc) throwError(ctx, "reduce: no active VM context");
        proto::ProtoContext scope(ctx->space, ctx);
        scope.resizeAutomaticLocals(2);
        SeqWalk w(&scope, 0, &scope, coll);
        releaseArg(args, ac == 2 ? 1 : 2);
        if (ac == 3) {
            scope.setAutomaticLocal(1, args->getAt(ctx, 1));
        } else {
            if (w.done()) {
                const proto::ProtoObject* r = cc->engine->invoke(&scope, f, nullptr, 0);
                scope.returnValue = r;
                return r;
            }
            scope.setAutomaticLocal(1, w.first(&scope));
            w.advance(&scope);
        }
        unsigned steps = 0;
        for (; !w.done(); w.advance(&scope)) {
            const proto::ProtoObject* two[2] = {scope.getAutomaticLocal(1), w.first(&scope)};
            scope.setAutomaticLocal(1, cc->engine->invoke(&scope, f, two, 2));
            if (++steps % 64 == 0) scope.safepoint();
        }
        const proto::ProtoObject* acc = scope.getAutomaticLocal(1);
        scope.returnValue = acc;
        return acc ? acc : PROTO_NONE;
    }
    const proto::ProtoList* lst = asSeqOrNull(ctx, coll);
    proto::proto_ulong n = lst ? lst->getSize(ctx) : 0;
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throwError(ctx, "reduce: no active VM context");

    const proto::ProtoObject* acc;
    proto::proto_ulong start;
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
    for (proto::proto_ulong i = start; i < n; ++i) {
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
                                    const proto::ProtoArguments* args,
                                    const proto::ProtoKeywordArguments*) {
    proto::proto_ulong ac = args ? args->getSize(ctx) : 0;
    if (ac != 2 && ac != 3)
        throwError(ctx, "subs: expects (subs s start) or (subs s start end)");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    if (!isStringLike(v))
        throwError(ctx, "subs: first arg must be a string");
    const proto::ProtoString* s = asProtoString(v);
    long long start = indexArg(ctx, args, 1, "subs");
    long long sz = static_cast<long long>(s->getSize(ctx));
    long long end = (ac == 3) ? indexArg(ctx, args, 2, "subs") : sz;
    if (start < 0 || start > sz || end < start || end > sz) {
        // The StringIndexOutOfBoundsException analogue, with the bounds as
        // written and Java's "begin B, end E, length L" message.
        throwClassed(ctx, "StringIndexOutOfBoundsException",
            "subs begin " +
            printedValue(ctx, args->getAt(ctx, 1)) + ", end " +
            (ac == 3 ? printedValue(ctx, args->getAt(ctx, 2)) : std::to_string(sz)) +
            ", length " + std::to_string(sz));
    }
    return reinterpret_cast<const proto::ProtoObject*>(
        s->getSlice(ctx, static_cast<int>(start), static_cast<int>(end)));
}

const proto::ProtoObject* prim_upper_case(proto::ProtoContext* ctx,
                                          const proto::ProtoObject*,
                                          const proto::ParentLink*,
                                          const proto::ProtoArguments* args,
                                          const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1)
        throwError(ctx, "upper-case: expects 1 arg");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    if (!isStringLike(v))
        throwError(ctx, "upper-case: arg must be a string");
    std::string raw = asProtoString(v)->toStdString(ctx);
    for (auto& c : raw) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return reinterpret_cast<const proto::ProtoObject*>(
        makeUtf8ProtoString(ctx, raw));
}

const proto::ProtoObject* prim_lower_case(proto::ProtoContext* ctx,
                                          const proto::ProtoObject*,
                                          const proto::ParentLink*,
                                          const proto::ProtoArguments* args,
                                          const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1)
        throwError(ctx, "lower-case: expects 1 arg");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    if (!isStringLike(v))
        throwError(ctx, "lower-case: arg must be a string");
    std::string raw = asProtoString(v)->toStdString(ctx);
    for (auto& c : raw) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return reinterpret_cast<const proto::ProtoObject*>(
        makeUtf8ProtoString(ctx, raw));
}

const proto::ProtoObject* prim_starts_with_p(proto::ProtoContext* ctx,
                                             const proto::ProtoObject*,
                                             const proto::ParentLink*,
                                             const proto::ProtoArguments* args,
                                             const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 2)
        throwError(ctx, "starts-with?: expects (starts-with? s prefix)");
    const proto::ProtoObject* sv = args->getAt(ctx, 0);
    const proto::ProtoObject* pv = args->getAt(ctx, 1);
    if (!isStringLike(sv) || !isStringLike(pv))
        throwError(ctx, "starts-with?: both args must be strings");
    std::string s = asProtoString(sv)->toStdString(ctx);
    std::string p = asProtoString(pv)->toStdString(ctx);
    return (s.rfind(p, 0) == 0) ? PROTO_TRUE : PROTO_FALSE;
}

const proto::ProtoObject* prim_ends_with_p(proto::ProtoContext* ctx,
                                           const proto::ProtoObject*,
                                           const proto::ParentLink*,
                                           const proto::ProtoArguments* args,
                                           const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 2)
        throwError(ctx, "ends-with?: expects (ends-with? s suffix)");
    const proto::ProtoObject* sv = args->getAt(ctx, 0);
    const proto::ProtoObject* pv = args->getAt(ctx, 1);
    if (!isStringLike(sv) || !isStringLike(pv))
        throwError(ctx, "ends-with?: both args must be strings");
    std::string s = asProtoString(sv)->toStdString(ctx);
    std::string p = asProtoString(pv)->toStdString(ctx);
    if (p.size() > s.size()) return PROTO_FALSE;
    return std::equal(p.rbegin(), p.rend(), s.rbegin()) ? PROTO_TRUE : PROTO_FALSE;
}

const proto::ProtoObject* prim_includes_p(proto::ProtoContext* ctx,
                                          const proto::ProtoObject*,
                                          const proto::ParentLink*,
                                          const proto::ProtoArguments* args,
                                          const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 2)
        throwError(ctx, "includes?: expects (includes? s sub)");
    const proto::ProtoObject* sv = args->getAt(ctx, 0);
    const proto::ProtoObject* pv = args->getAt(ctx, 1);
    if (!isStringLike(sv) || !isStringLike(pv))
        throwError(ctx, "includes?: both args must be strings");
    std::string s = asProtoString(sv)->toStdString(ctx);
    std::string p = asProtoString(pv)->toStdString(ctx);
    return (s.find(p) != std::string::npos) ? PROTO_TRUE : PROTO_FALSE;
}

const proto::ProtoObject* prim_index_of(proto::ProtoContext* ctx,
                                        const proto::ProtoObject*,
                                        const proto::ParentLink*,
                                        const proto::ProtoArguments* args,
                                        const proto::ProtoKeywordArguments*) {
    proto::proto_ulong ac = args ? args->getSize(ctx) : 0;
    if (ac != 2 && ac != 3)
        throwError(ctx, "index-of: expects (index-of s sub) or (index-of s sub from)");
    const proto::ProtoObject* sv = args->getAt(ctx, 0);
    const proto::ProtoObject* pv = args->getAt(ctx, 1);
    if (!isStringLike(sv) || !isStringLike(pv))
        throwError(ctx, "index-of: first two args must be strings");
    std::string s = asProtoString(sv)->toStdString(ctx);
    std::string p = asProtoString(pv)->toStdString(ctx);
    // The start position follows Java's String.indexOf, at any magnitude: a
    // negative start searches from 0, and a start past the end searches from
    // the end (only the empty string is found there).
    long long from = (ac == 3) ? indexArg(ctx, args, 2, "index-of") : 0;
    if (from < 0) from = 0;
    if (static_cast<unsigned long long>(from) > s.size())
        from = static_cast<long long>(s.size());
    std::size_t found = s.find(p, static_cast<std::size_t>(from));
    if (found == std::string::npos) return PROTO_NONE;
    return ctx->fromLong(static_cast<long long>(found));
}

// (identical? x y) — the same object (pointer equality), as in Clojure.
const proto::ProtoObject* prim_identical_p(proto::ProtoContext* ctx,
                                           const proto::ProtoObject*,
                                           const proto::ParentLink*,
                                           const proto::ProtoArguments* args,
                                           const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 2) throwError(ctx, "identical?: expects 2 args");
    return args->getAt(ctx, 0) == args->getAt(ctx, 1) ? PROTO_TRUE : PROTO_FALSE;
}

// Regular expressions (Regex.h) ---------------------------------------------

// Argument `i` as a compiled pattern; `who` names the function in the error.
const CompiledPattern* patternArg(proto::ProtoContext* ctx, const proto::ProtoArguments* args,
                                  proto::proto_ulong i, const char* who) {
    const proto::ProtoObject* v = args->getAt(ctx, static_cast<int>(i));
    if (isPattern(ctx, v)) return patternOf(ctx, v);
    throwClassed(ctx, "ClassCastException", std::string(who) + " expects a pattern, got " +
                                            valueTypeName(ctx, v));
}

std::string stringArgOf(proto::ProtoContext* ctx, const proto::ProtoArguments* args,
                        proto::proto_ulong i, const char* who) {
    const proto::ProtoObject* v = args->getAt(ctx, static_cast<int>(i));
    if (isStringLike(v)) return asProtoString(v)->toStdString(ctx);
    throwClassed(ctx, "ClassCastException", std::string(who) + " expects a string, got " +
                                            valueTypeName(ctx, v));
}

// What re-find and re-matches answer for a match: the matched string when
// the pattern has no groups, else the vector [whole g1 g2 ...] with nil for
// a group that did not participate, as in Clojure.
const proto::ProtoObject* matchValue(proto::ProtoContext* ctx, const std::smatch& m,
                                     unsigned groups) {
    if (groups == 0)
        return reinterpret_cast<const proto::ProtoObject*>(
            makeUtf8ProtoString(ctx, m.str(0)));
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(1);
    scope.setAutomaticLocal(0, scope.newList()->asObject(&scope));
    for (unsigned g = 0; g <= groups; ++g) {
        const proto::ProtoObject* part = m[g].matched
            ? reinterpret_cast<const proto::ProtoObject*>(makeUtf8ProtoString(&scope, m.str(g)))
            : PROTO_NONE;
        scope.setAutomaticLocal(0, scope.getAutomaticLocal(0)->asList(&scope)
            ->appendLast(&scope, part)->asObject(&scope));
    }
    return newVector(&scope, scope.getAutomaticLocal(0)->asList(&scope));
}

// (re-pattern s) — the pattern of a string; a pattern is itself.
const proto::ProtoObject* prim_re_pattern(proto::ProtoContext* ctx,
                                          const proto::ProtoObject*,
                                          const proto::ParentLink*,
                                          const proto::ProtoArguments* args,
                                          const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1) throwError(ctx, "re-pattern: expects 1 arg");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    if (isPattern(ctx, v)) return v;
    const std::string src = stringArgOf(ctx, args, 0, "re-pattern");
    try {
        return makePattern(ctx, src);
    } catch (const PatternSyntaxError& e) {
        throwClassed(ctx, "PatternSyntaxException", e.what());
    }
}

// Matcher attributes (re-matcher, re-find on a matcher, re-groups).
const proto::ProtoString* mPatternKey(proto::ProtoContext* c) { return proto::ProtoString::createSymbol(c, "__m_pattern__"); }
const proto::ProtoString* mSubjectKey(proto::ProtoContext* c) { return proto::ProtoString::createSymbol(c, "__m_subject__"); }
const proto::ProtoString* mPosKey(proto::ProtoContext* c)     { return proto::ProtoString::createSymbol(c, "__m_pos__"); }
const proto::ProtoString* mGroupsKey(proto::ProtoContext* c)  { return proto::ProtoString::createSymbol(c, "__m_groups__"); }

// (re-matcher re s) — a matcher over `s`: each (re-find m) answers the next
// match, and (re-groups m) the groups of the last one.
const proto::ProtoObject* prim_re_matcher(proto::ProtoContext* ctx,
                                          const proto::ProtoObject*,
                                          const proto::ParentLink*,
                                          const proto::ProtoArguments* args,
                                          const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 2) throwError(ctx, "re-matcher: expects (re-matcher re s)");
    patternArg(ctx, args, 0, "re-matcher");
    stringArgOf(ctx, args, 1, "re-matcher");
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(1);
    scope.setAutomaticLocal(0, matcherPrototype()->newChild(&scope, /*isMutable=*/true));
    proto::ProtoObject* m = const_cast<proto::ProtoObject*>(scope.getAutomaticLocal(0));
    m->setAttribute(&scope, mPatternKey(&scope), args->getAt(&scope, 0));
    m->setAttribute(&scope, mSubjectKey(&scope), args->getAt(&scope, 1));
    m->setAttribute(&scope, mPosKey(&scope), scope.fromLong(0));
    return scope.getAutomaticLocal(0);
}

// The next match of matcher `mv`, as re-find answers it, or nil; records the
// groups for re-groups and moves the position past the match (one byte
// further after an empty match, so a find always makes progress).
const proto::ProtoObject* matcherFind(proto::ProtoContext* ctx, const proto::ProtoObject* mv) {
    proto::ProtoObject* m = const_cast<proto::ProtoObject*>(mv);
    const CompiledPattern* cp = patternOf(ctx, m->getOwnAttributeDirect(ctx, mPatternKey(ctx)));
    const std::string subject = asProtoString(m->getOwnAttributeDirect(ctx, mSubjectKey(ctx)))->toStdString(ctx);
    const long long pos = m->getOwnAttributeDirect(ctx, mPosKey(ctx))->asLong(ctx);
    if (pos > static_cast<long long>(subject.size())) return PROTO_NONE;
    std::smatch match;
    const auto flags = continuationFlags(cp, subject, static_cast<std::size_t>(pos));
    guardRecursiveMatch(cp, subject.size() - static_cast<std::size_t>(pos));
    if (!runRegex(cp, [&] { return std::regex_search(subject.begin() + pos, subject.end(), match,
                                                     cp->regex, flags); })) {
        m->setAttribute(ctx, mPosKey(ctx), ctx->fromLong(static_cast<long long>(subject.size()) + 1));
        return PROTO_NONE;
    }
    const long long start = pos + match.position(0);
    long long next = start + match.length(0);
    if (match.length(0) == 0) ++next;
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(1);
    scope.setAutomaticLocal(0, matchValue(&scope, match, cp->groups));
    m->setAttribute(&scope, mGroupsKey(&scope), scope.getAutomaticLocal(0));
    m->setAttribute(&scope, mPosKey(&scope), scope.fromLong(next));
    return scope.getAutomaticLocal(0);
}

// (re-find re s) — the first match in `s`, or nil; (re-find m) — the next
// match of a matcher.
const proto::ProtoObject* prim_re_find(proto::ProtoContext* ctx,
                                       const proto::ProtoObject*,
                                       const proto::ParentLink*,
                                       const proto::ProtoArguments* args,
                                       const proto::ProtoKeywordArguments*) {
    const proto::proto_ulong n = args ? args->getSize(ctx) : 0;
    if (n == 1) {
        const proto::ProtoObject* m = args->getAt(ctx, 0);
        if (!isMatcher(ctx, m))
            throwClassed(ctx, "ClassCastException", std::string("re-find expects a matcher, got ") +
                                                    valueTypeName(ctx, m));
        return matcherFind(ctx, m);
    }
    if (n != 2) throwError(ctx, "re-find: expects (re-find re s) or (re-find matcher)");
    const CompiledPattern* cp = patternArg(ctx, args, 0, "re-find");
    const std::string subject = stringArgOf(ctx, args, 1, "re-find");
    std::smatch match;
    guardRecursiveMatch(cp, subject.size());
    if (!runRegex(cp, [&] { return std::regex_search(subject, match, cp->regex); })) return PROTO_NONE;
    return matchValue(ctx, match, cp->groups);
}

// (re-matches re s) — the match of the whole of `s`, or nil.
const proto::ProtoObject* prim_re_matches(proto::ProtoContext* ctx,
                                          const proto::ProtoObject*,
                                          const proto::ParentLink*,
                                          const proto::ProtoArguments* args,
                                          const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 2) throwError(ctx, "re-matches: expects (re-matches re s)");
    const CompiledPattern* cp = patternArg(ctx, args, 0, "re-matches");
    const std::string subject = stringArgOf(ctx, args, 1, "re-matches");
    std::smatch match;
    guardRecursiveMatch(cp, subject.size());
    if (!runRegex(cp, [&] { return std::regex_match(subject, match, cp->regex); })) return PROTO_NONE;
    return matchValue(ctx, match, cp->groups);
}

// (re-seq re s) — every successive match of `re` in `s`, or nil when there is
// none. Clojure answers a lazy seq; here it is a list, built eagerly.
// One find of re-seq from byte `pos` of `subject`, as Java's Matcher.find:
// the next match and where the following search starts (after an empty
// match, one byte further on, so the walk always advances). The match value
// and the next lazy seq are consed; nil when nothing more matches.
const proto::ProtoObject* reSeqFrom(proto::ProtoContext* ctx, const proto::ProtoObject* pattern,
                                    const proto::ProtoObject* subjectBox, std::size_t pos) {
    const CompiledPattern* cp = patternOf(ctx, pattern);
    const auto* subject = static_cast<const std::string*>(
        subjectBox->asExternalPointer(ctx)->getPointer(ctx));
    if (pos > subject->size()) return PROTO_NONE;
    std::smatch m;
    const auto flags = continuationFlags(cp, *subject, pos);
    if (!runRegex(cp, [&] { return std::regex_search(subject->cbegin() + static_cast<std::ptrdiff_t>(pos),
                                                     subject->cend(), m, cp->regex, flags); }))
        return PROTO_NONE;
    const std::size_t start = static_cast<std::size_t>(m[0].first - subject->cbegin());
    const std::size_t end = start + static_cast<std::size_t>(m.length(0));
    const std::size_t next = m.length(0) == 0 ? end + 1 : end;
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(2);
    scope.setAutomaticLocal(0, matchValue(&scope, m, cp->groups));
    scope.setAutomaticLocal(1, lazyStep(&scope, StepKind::ReSeq, pattern, subjectBox,
                                        scope.fromLong(static_cast<long long>(next))));
    const proto::ProtoObject* r = newCons(&scope, scope.getAutomaticLocal(0), scope.getAutomaticLocal(1));
    scope.returnValue = r;
    return r;
}

// The ReSeq step (LazySeq.h): o.a = the pattern, o.b = the subject's bytes
// (an external pointer to a std::string, freed with it), o.c = the position.
const proto::ProtoObject* stepReSeq(proto::ProtoContext* ctx, const proto::ProtoObject*,
                                    const StepOperands& o) {
    return reSeqFrom(ctx, o.a, o.b, static_cast<std::size_t>(o.c->asLong(ctx)));
}

// (re-seq re s) — the successive matches of `re` in `s` as a lazy seq, each
// as re-find answers it; nil when nothing matches. The first match is found
// now and the next ones as the seq is read, as in Clojure. The subject is
// copied once into a buffer the seq shares.
const proto::ProtoObject* prim_re_seq(proto::ProtoContext* ctx,
                                      const proto::ProtoObject*,
                                      const proto::ParentLink*,
                                      const proto::ProtoArguments* args,
                                      const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 2) throwError(ctx, "re-seq: expects (re-seq re s)");
    const CompiledPattern* cp = patternArg(ctx, args, 0, "re-seq");
    std::string subject = stringArgOf(ctx, args, 1, "re-seq");
    guardRecursiveMatch(cp, subject.size());
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(1);
    scope.setAutomaticLocal(0, scope.fromExternalPointer(
        new std::string(std::move(subject)),
        [](void* p) { delete static_cast<std::string*>(p); }));
    const proto::ProtoObject* r = reSeqFrom(&scope, args->getAt(&scope, 0), scope.getAutomaticLocal(0), 0);
    scope.returnValue = r;
    return r;
}

// (re-groups m) — the groups of the matcher's last successful find.
const proto::ProtoObject* prim_re_groups(proto::ProtoContext* ctx,
                                         const proto::ProtoObject*,
                                         const proto::ParentLink*,
                                         const proto::ProtoArguments* args,
                                         const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1) throwError(ctx, "re-groups: expects 1 arg");
    const proto::ProtoObject* m = args->getAt(ctx, 0);
    if (!isMatcher(ctx, m))
        throwClassed(ctx, "ClassCastException", std::string("re-groups expects a matcher, got ") +
                                                valueTypeName(ctx, m));
    const proto::ProtoObject* g = m->getOwnAttributeDirect(ctx, mGroupsKey(ctx));
    if (!g) throwClassed(ctx, "IllegalStateException", "No match found");
    return g;
}

// The replacement text for one match: a fn of the match (re-find's value)
// whose result is rendered by str, or a string where `$n` is group n (the
// longest number that names a group, as Java reads it) and `\x` is x.
void appendReplacement(proto::ProtoContext* ctx, std::string& out, const std::smatch& m,
                       unsigned groups, const proto::ProtoObject* repl) {
    if (!isStringLike(repl)) {
        const ActiveCallContext* cc = activeCallContext();
        if (!cc) throwError(ctx, "replace: no active VM context");
        proto::ProtoContext scope(ctx->space, ctx);
        scope.resizeAutomaticLocals(1);
        scope.setAutomaticLocal(0, matchValue(&scope, m, groups));
        const proto::ProtoObject* one[1] = {scope.getAutomaticLocal(0)};
        appendStr(&scope, out, cc->engine->invoke(&scope, repl, one, 1));
        return;
    }
    const std::string r = asProtoString(repl)->toStdString(ctx);
    for (std::size_t i = 0; i < r.size(); ++i) {
        const char c = r[i];
        if (c == '\\' && i + 1 < r.size()) { out += r[++i]; continue; }
        if (c != '$') { out += c; continue; }
        if (i + 1 >= r.size() || !std::isdigit(static_cast<unsigned char>(r[i + 1])))
            throwClassed(ctx, "IllegalArgumentException", "Illegal group reference in replacement: " + r);
        unsigned g = static_cast<unsigned>(r[++i] - '0');
        while (i + 1 < r.size() && std::isdigit(static_cast<unsigned char>(r[i + 1]))) {
            const unsigned next = g * 10 + static_cast<unsigned>(r[i + 1] - '0');
            if (next > groups) break;
            g = next;
            ++i;
        }
        if (g > groups)
            throwClassed(ctx, "IndexOutOfBoundsException", "No group " + std::to_string(g));
        if (m[g].matched) out += m.str(g);
    }
}

// `s` with the matches of `cp` replaced (all, or the first only).
const proto::ProtoObject* replaceWithPattern(proto::ProtoContext* ctx, const std::string& s,
                                             const CompiledPattern* cp,
                                             const proto::ProtoObject* repl, bool firstOnly) {
    guardRecursiveMatch(cp, s.size());
    std::string out;
    out.reserve(s.size());
    auto last = s.begin();
    forEachMatch(s, cp, [&](const std::smatch& m) {
        out.append(last, m[0].first);
        appendReplacement(ctx, out, m, cp->groups, repl);
        last = m[0].second;
        return !firstOnly;
    });
    out.append(last, s.end());
    return reinterpret_cast<const proto::ProtoObject*>(makeUtf8ProtoString(ctx, out));
}

// (replace-first s match replacement) — replace with only the first match
// replaced: a string match, or a pattern with a string or fn replacement.
const proto::ProtoObject* prim_replace_first(proto::ProtoContext* ctx,
                                             const proto::ProtoObject*,
                                             const proto::ParentLink*,
                                             const proto::ProtoArguments* args,
                                             const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 3)
        throwError(ctx, "replace-first: expects (replace-first s match replacement)");
    const std::string s = stringArgOf(ctx, args, 0, "replace-first");
    const proto::ProtoObject* mv = args->getAt(ctx, 1);
    if (isPattern(ctx, mv))
        return replaceWithPattern(ctx, s, patternOf(ctx, mv), args->getAt(ctx, 2), true);
    const std::string m = stringArgOf(ctx, args, 1, "replace-first");
    const std::string r = stringArgOf(ctx, args, 2, "replace-first");
    const std::size_t at = s.find(m);
    if (at == std::string::npos) return args->getAt(ctx, 0);
    return reinterpret_cast<const proto::ProtoObject*>(makeUtf8ProtoString(
        ctx, s.substr(0, at) + r + s.substr(at + m.size())));
}

// (split s re) / (split s re limit) with a pattern, as Java's String.split
// and Clojure's clojure.string/split: a vector of the pieces; a match of
// zero width at the start makes no leading empty piece; with no limit (or 0)
// trailing empty pieces are dropped, with a positive limit at most `limit`
// pieces are made (the last holds the rest), with a negative one all are kept.
const proto::ProtoObject* splitWithPattern(proto::ProtoContext* ctx, const std::string& s,
                                           const CompiledPattern* cp, long long limit) {
    guardRecursiveMatch(cp, s.size());
    std::vector<std::string> pieces;
    std::size_t from = 0;
    forEachMatch(s, cp, [&](const std::smatch& m) {
        if (limit > 0 && static_cast<long long>(pieces.size()) == limit - 1) return false;
        const std::size_t at = static_cast<std::size_t>(m[0].first - s.cbegin());
        const std::size_t len = static_cast<std::size_t>(m.length(0));
        if (at == 0 && len == 0) return true;        // no leading empty piece
        if (len == 0 && at >= s.size()) return false; // the empty match at the end
        pieces.push_back(s.substr(from, at - from));
        from = at + len;
        return true;
    });
    pieces.push_back(s.substr(from));
    if (limit == 0)
        while (pieces.size() > 1 && pieces.back().empty()) pieces.pop_back();
    if (limit == 0 && pieces.size() == 1 && pieces[0].empty() && !s.empty()) pieces.clear();
    ListBuilder out(ctx);
    for (const std::string& p : pieces)
        out.push(reinterpret_cast<const proto::ProtoObject*>(
            makeUtf8ProtoString(out.context(), p)));
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(1);
    scope.setAutomaticLocal(0, out.finish());
    return newVector(&scope, scope.getAutomaticLocal(0)->asList(&scope));
}

const proto::ProtoObject* prim_replace(proto::ProtoContext* ctx,
                                       const proto::ProtoObject*,
                                       const proto::ParentLink*,
                                       const proto::ProtoArguments* args,
                                       const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 3)
        throwError(ctx, "replace: expects (replace s match replacement)");
    const proto::ProtoObject* sv = args->getAt(ctx, 0);
    const proto::ProtoObject* mv = args->getAt(ctx, 1);
    const proto::ProtoObject* rv = args->getAt(ctx, 2);
    // A pattern match, with a string replacement ($1 is group 1) or a fn of
    // the match, as clojure.string/replace.
    if (isPattern(ctx, mv) && isStringLike(sv))
        return replaceWithPattern(ctx, asProtoString(sv)->toStdString(ctx), patternOf(ctx, mv), rv, false);
    if (!isStringLike(sv) || !isStringLike(mv) || !isStringLike(rv))
        throwError(ctx, "replace: all three args must be strings");
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
        makeUtf8ProtoString(ctx, out));
}

// (join sep coll) — concatenate `coll` items, separator between each.
// (join coll) — same with empty separator.
const proto::ProtoObject* prim_join(proto::ProtoContext* ctx,
                                    const proto::ProtoObject*,
                                    const proto::ParentLink*,
                                    const proto::ProtoArguments* args,
                                    const proto::ProtoKeywordArguments*) {
    proto::proto_ulong ac = args ? args->getSize(ctx) : 0;
    if (ac != 1 && ac != 2)
        throwError(ctx, "join: expects (join coll) or (join sep coll)");
    std::string sep;
    const proto::ProtoObject* coll;
    if (ac == 2) {
        const proto::ProtoObject* sv = args->getAt(ctx, 0);
        if (!isStringLike(sv))
            throwError(ctx, "join: separator must be a string");
        sep = asProtoString(sv)->toStdString(ctx);
        coll = args->getAt(ctx, 1);
    } else {
        coll = args->getAt(ctx, 0);
    }
    const proto::ProtoList* lst = asSeqOrNull(ctx, coll);
    std::string text;
    if (lst) {
        proto::proto_ulong n = lst->getSize(ctx);
        for (proto::proto_ulong i = 0; i < n; ++i) {
            if (i > 0) text += sep;
            appendStr(ctx, text, lst->getAt(ctx, static_cast<int>(i)));
        }
    }
    return reinterpret_cast<const proto::ProtoObject*>(
        makeUtf8ProtoString(ctx, text));
}

// (split s delim) — returns a list of substrings split by `delim`.
const proto::ProtoObject* prim_split(proto::ProtoContext* ctx,
                                     const proto::ProtoObject*,
                                     const proto::ParentLink*,
                                     const proto::ProtoArguments* args,
                                     const proto::ProtoKeywordArguments*) {
    const proto::proto_ulong ac = args ? args->getSize(ctx) : 0;
    if (ac != 2 && ac != 3)
        throwError(ctx, "split: expects (split s re) or (split s re limit)");
    const proto::ProtoObject* sv = args->getAt(ctx, 0);
    const proto::ProtoObject* dv = args->getAt(ctx, 1);
    if (isPattern(ctx, dv)) {
        const std::string subject = stringArgOf(ctx, args, 0, "split");
        return splitWithPattern(ctx, subject, patternOf(ctx, dv),
                                ac == 3 ? indexArg(ctx, args, 2, "split") : 0);
    }
    if (ac != 2) throwError(ctx, "split: a limit needs a pattern");
    if (!isStringLike(sv) || !isStringLike(dv))
        throwError(ctx, "split: both args must be strings");
    std::string s = asProtoString(sv)->toStdString(ctx);
    std::string d = asProtoString(dv)->toStdString(ctx);
    // One piece per delimiter occurrence: the result grows with the input
    // string, so the build is chunked.
    ListBuilder out(ctx);
    if (d.empty()) {
        // Split into individual codepoints would be ideal; v0.15
        // splits per byte.
        for (char c : s) {
            std::string one(1, c);
            out.push(reinterpret_cast<const proto::ProtoObject*>(
                makeUtf8ProtoString(out.context(), one)));
        }
        return out.finish();
    }
    std::size_t pos = 0;
    while (true) {
        std::size_t found = s.find(d, pos);
        std::string piece = (found == std::string::npos)
            ? s.substr(pos)
            : s.substr(pos, found - pos);
        out.push(reinterpret_cast<const proto::ProtoObject*>(
            makeUtf8ProtoString(out.context(), piece)));
        if (found == std::string::npos) break;
        pos = found + d.size();
    }
    return out.finish();
}

static bool isAsciiWs(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

const proto::ProtoObject* prim_trim(proto::ProtoContext* ctx,
                                    const proto::ProtoObject*,
                                    const proto::ParentLink*,
                                    const proto::ProtoArguments* args,
                                    const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1)
        throwError(ctx, "trim: expects 1 arg");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    if (!isStringLike(v))
        throwError(ctx, "trim: arg must be a string");
    std::string s = asProtoString(v)->toStdString(ctx);
    std::size_t a = 0, b = s.size();
    while (a < b && isAsciiWs(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && isAsciiWs(static_cast<unsigned char>(s[b - 1]))) --b;
    return reinterpret_cast<const proto::ProtoObject*>(
        makeUtf8ProtoString(ctx, s.substr(a, b - a)));
}

const proto::ProtoObject* prim_triml(proto::ProtoContext* ctx,
                                     const proto::ProtoObject*,
                                     const proto::ParentLink*,
                                     const proto::ProtoArguments* args,
                                     const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1)
        throwError(ctx, "triml: expects 1 arg");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    if (!isStringLike(v))
        throwError(ctx, "triml: arg must be a string");
    std::string s = asProtoString(v)->toStdString(ctx);
    std::size_t a = 0;
    while (a < s.size() && isAsciiWs(static_cast<unsigned char>(s[a]))) ++a;
    return reinterpret_cast<const proto::ProtoObject*>(
        makeUtf8ProtoString(ctx, s.substr(a)));
}

const proto::ProtoObject* prim_trimr(proto::ProtoContext* ctx,
                                     const proto::ProtoObject*,
                                     const proto::ParentLink*,
                                     const proto::ProtoArguments* args,
                                     const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1)
        throwError(ctx, "trimr: expects 1 arg");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    if (!isStringLike(v))
        throwError(ctx, "trimr: arg must be a string");
    std::string s = asProtoString(v)->toStdString(ctx);
    std::size_t b = s.size();
    while (b > 0 && isAsciiWs(static_cast<unsigned char>(s[b - 1]))) --b;
    return reinterpret_cast<const proto::ProtoObject*>(
        makeUtf8ProtoString(ctx, s.substr(0, b)));
}

const proto::ProtoObject* prim_blank_p(proto::ProtoContext* ctx,
                                       const proto::ProtoObject*,
                                       const proto::ParentLink*,
                                       const proto::ProtoArguments* args,
                                       const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1)
        throwError(ctx, "blank?: expects 1 arg");
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
                                        const proto::ProtoArguments* args,
                                        const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1)
        throwError(ctx, "string?: expects 1 arg");
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
                                    const proto::ProtoArguments* args,
                                    const proto::ProtoKeywordArguments*) {
    proto::proto_ulong n = args ? args->getSize(ctx) : 0;
    if (n != 1)
        throwError(ctx, "atom: expects (atom initial-value)");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throwError(ctx, "atom: no active VM context");
    proto::ProtoObject* a = const_cast<proto::ProtoObject*>(
        cc->atomMarkerProto->newChild(ctx, /*isMutable=*/true));
    a->setAttribute(ctx, cc->valueKey, args->getAt(ctx, 0));
    return a;
}

const proto::ProtoObject* prim_atom_p(proto::ProtoContext* ctx,
                                      const proto::ProtoObject*,
                                      const proto::ParentLink*,
                                      const proto::ProtoArguments* args,
                                      const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1)
        throwError(ctx, "atom?: expects 1 arg");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throwError(ctx, "atom?: no active VM context");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    if (!v) return PROTO_FALSE;
    return (v->getPrototype(ctx) == cc->atomMarkerProto) ? PROTO_TRUE : PROTO_FALSE;
}

// The attribute under which a future, or a pmap element, records the error
// its body raised: the exception value (Exceptions.h). Absent when the body
// returned normally.
const proto::ProtoString* threadErrorKey(proto::ProtoContext* ctx) {
    return proto::ProtoString::createSymbol(ctx, "__error__");
}

// Records the error being handled on `holder` (a future or a pmap element)
// under threadErrorKey: the value of a ProtoException (a Clojure `throw`, a
// protoCore error, a module's error), or the classed exception of any other
// C++ error (caughtErrorValue). A raised nil, which only foreign code can
// raise, is recorded as a RuntimeException so that a failure is never
// mistaken for a result. Call only from inside a catch block, where the
// in-flight exception is still pinned. `holder` is mutable and rooted by the
// caller.
void recordThreadError(proto::ProtoContext* ctx, const proto::ProtoObject* holder) {
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(1);
    scope.setAutomaticLocal(0, caughtErrorValue(&scope));
    if (scope.getAutomaticLocal(0) == PROTO_NONE) {
        scope.setAutomaticLocal(0, exceptionFromError(
            &scope, std::runtime_error("a foreign error raised nil")));
    }
    const_cast<proto::ProtoObject*>(holder)->setAttribute(
        &scope, threadErrorKey(&scope), scope.getAutomaticLocal(0));
}

// The value the body of `holder` (a future or a pmap element) raised, or
// nullptr when it returned normally. Reachable from `holder`.
const proto::ProtoObject* threadFailure(proto::ProtoContext* ctx,
                                        const proto::ProtoObject* holder) {
    const proto::ProtoObject* error = holder->getAttribute(ctx, threadErrorKey(ctx));
    return (error && error != PROTO_NONE) ? error : nullptr;
}

// JVM Clojure's deref of a failed future raises a
// java.util.concurrent.ExecutionException whose cause is the exception the
// body raised and whose message is the cause's toString, "<class>:
// <message>" ("ArithmeticException: Divide by zero"). `cause` must be rooted
// by the caller.
[[noreturn]]
void throwExecutionException(proto::ProtoContext* ctx,
                             const proto::ProtoObject* cause) {
    throwClassed(ctx, "ExecutionException", describeThrown(ctx, cause),
                 nullptr, cause);
}

const proto::ProtoObject* prim_deref(proto::ProtoContext* ctx,
                                     const proto::ProtoObject*,
                                     const proto::ParentLink*,
                                     const proto::ProtoArguments* args,
                                     const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1)
        throwError(ctx, "deref: expects 1 arg");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throwError(ctx, "deref: no active VM context");
    const proto::ProtoObject* a = args->getAt(ctx, 0);
    if (!a) throwError(ctx, "deref: nil");
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
                    // See the note at shutdownFuturesImpl: a joining thread
                    // must leave protoCore's running set or the future it is
                    // waiting for can never get a collection cycle.
                    // Kept although protoCore's ProtoThread::join now brackets itself
                    // (2026-09-25, protoCore >= 2.3).  Nesting is provably idempotent:
                    // unmanagedDepth is a counter and only the outermost pair moves
                    // parkedThreads, so this guard costs one atomic increment.  It is kept
                    // because it protects against a version skew protoClojure's build CANNOT
                    // detect -- the build checks SOVERSION (3) and the fix changed no ABI, so a
                    // protoClojure linked against protoCore 2.2.0 would deadlock here with no
                    // warning.  Verified by experiment: with all four guards removed,
                    // tests/cli/blocking-joins-park-for-gc.sh passes in 8.3 s against a fixed
                    // kernel and is killed at 90 s against protoCore 9e85a4c0.  Remove these
                    // four guards once every embedder requires protoCore 2.3.
                    proto::ProtoContext::UnmanagedScope parked(ctx);
                    t->join(ctx);
                }
            }
        }
        // A body that raised an error makes every deref raise it, wrapped as
        // JVM Clojure's ExecutionException.
        if (const proto::ProtoObject* cause = threadFailure(ctx, a))
            throwExecutionException(ctx, cause);
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
        // Poll. ProtoThread::sleepFor takes this thread out of the
        // stop-the-world quorum for the sleep, so a collection can run
        // while it waits, and parks it on the way back if one is in
        // progress. The 1 ms sleep amortises wake-up under realistic
        // deliver latencies. Delivery is one compare-and-set
        // (deliverPromise), so the read that finds the promise delivered
        // also yields its value.
        for (;;) {
            bool delivered = false;
            const proto::ProtoObject* v =
                promiseValue(ctx, cc->valueKey, a, &delivered);
            if (delivered) return v;
            proto::ProtoThread::sleepFor(ctx, 1000);
        }
    }
    throwError(ctx, "deref: not an atom, future, or promise");
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
    // Pin the watches map as soon as it is read: another thread may replace
    // the atom's watches (add-watch, remove-watch) while the watches run.
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(2);
    constexpr unsigned int kSlotWatchesMap = 0;
    constexpr unsigned int kSlotWatchList  = 1;
    const proto::ProtoObject* watchesObj =
        atomObj->getAttribute(&scope, cc->watchesKey);
    if (!isMap(watchesObj)) return;
    scope.setAutomaticLocal(kSlotWatchesMap, watchesObj);

    // Collect (key fn key fn ...) first, then call the watches after the
    // walk. On a map of more than 3 entries protoCore walks inside a GC
    // critical section, where only structures may be built: user code there
    // would skip heap-limit checkpoints and never park for collection. Watch
    // order is the map's unspecified walk order, as on the JVM.
    scope.setAutomaticLocal(kSlotWatchList, scope.newList()->asObject(&scope));
    mapForEach(&scope, watchesObj, &scope,
        [](proto::ProtoContext* c, void* self,
           const proto::ProtoObject* k, const proto::ProtoObject* f) {
            auto* s = static_cast<proto::ProtoContext*>(self);
            s->setAutomaticLocal(kSlotWatchList,
                s->getAutomaticLocal(kSlotWatchList)->asList(c)
                    ->appendLast(c, k)->appendLast(c, f)->asObject(c));
        });

    // The list stays rooted in its slot while the watches run.
    const proto::ProtoList* watches =
        scope.getAutomaticLocal(kSlotWatchList)->asList(&scope);
    const proto::proto_ulong n = watches->getSize(&scope);
    for (proto::proto_ulong i = 0; i + 1 < n; i += 2) {
        const proto::ProtoObject* four[4] = {
            watches->getAt(&scope, static_cast<int>(i)), atomObj,
            oldV ? oldV : PROTO_NONE,
            newV ? newV : PROTO_NONE
        };
        try {
            DebugCatchBarrier swallowed;   // nothing the watch throws reaches the caller
            cc->engine->invoke(&scope,
                               watches->getAt(&scope, static_cast<int>(i + 1)),
                               four, 4);
        } catch (...) {
            // v0.18: swallow exceptions in a watcher; capturing them is a
            // follow-up.
        }
    }
}

const proto::ProtoObject* prim_reset_bang(proto::ProtoContext* ctx,
                                          const proto::ProtoObject*,
                                          const proto::ParentLink*,
                                          const proto::ProtoArguments* args,
                                          const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 2)
        throwError(ctx, "reset!: expects (reset! atom new-value)");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throwError(ctx, "reset!: no active VM context");
    const proto::ProtoObject* a = args->getAt(ctx, 0);
    if (!a || a->getPrototype(ctx) != cc->atomMarkerProto)
        throwError(ctx, "reset!: not an atom");
    const proto::ProtoObject* nv = args->getAt(ctx, 1);
    // One compare-and-set per attempt, so the old value handed to the
    // watches is exactly the value this reset! replaced, even when other
    // threads write the atom concurrently. The old value is referenced only
    // by this function once replaced, and the compare-and-set and the
    // watches allocate: it is pinned in a child context slot as soon as it
    // is read, with no allocation in between.
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(1);
    for (;;) {
        const proto::ProtoObject* old =
            a->getOwnAttributeDirect(&scope, cc->valueKey);
        scope.setAutomaticLocal(0, old ? old : PROTO_NONE);
        if (a->setAttributeIfEqual(&scope, cc->valueKey, old, nv)) {
            fireWatches(&scope, cc, a, old ? old : PROTO_NONE, nv);
            return nv;
        }
    }
}

const proto::ProtoObject* prim_swap_bang(proto::ProtoContext* ctx,
                                         const proto::ProtoObject*,
                                         const proto::ParentLink*,
                                         const proto::ProtoArguments* args,
                                         const proto::ProtoKeywordArguments*) {
    proto::proto_ulong n = args ? args->getSize(ctx) : 0;
    if (n < 2)
        throwError(ctx, "swap!: expects (swap! atom f & args)");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throwError(ctx, "swap!: no active VM context");
    const proto::ProtoObject* a = args->getAt(ctx, 0);
    if (!a || a->getPrototype(ctx) != cc->atomMarkerProto)
        throwError(ctx, "swap!: not an atom");
    const proto::ProtoObject* f = args->getAt(ctx, 1);

    // Build (old + extras) buffer once; old gets overwritten per attempt.
    if (n - 2 > 15)
        throwError(ctx, "swap!: >15 extra args not supported");
    const unsigned int extras = static_cast<unsigned int>(n - 2);
    const proto::ProtoObject* buf[17];
    for (unsigned int i = 0; i < extras; ++i) {
        buf[i + 1] = args->getAt(ctx, static_cast<int>(2 + i));
    }

    // CAS retry loop. protoCore guarantees `getOwnAttributeDirect`
    // observes the value through the current shard root; if a
    // concurrent writer installs a new snapshot before our CAS
    // completes, setAttributeIfEqual returns false and we recompute
    // (f old ...) against the fresh `old`. Lock-free, no mutex.
    //
    // GC rooting: another thread may replace the atom's value, dropping the
    // last reference to `old`, while `f`, the compare-and-set and the
    // watches allocate; and the callee returns `neu` unrooted. Both are
    // pinned in a child context's slots as soon as they are obtained, with
    // no allocation in between.
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(2);
    constexpr unsigned int kSlotOld = 0;
    constexpr unsigned int kSlotNew = 1;
    for (;;) {
        const proto::ProtoObject* old =
            a->getOwnAttributeDirect(&scope, cc->valueKey);
        scope.setAutomaticLocal(kSlotOld, old ? old : PROTO_NONE);
        buf[0] = old ? old : PROTO_NONE;
        const proto::ProtoObject* neu =
            cc->engine->invoke(&scope, f, buf, extras + 1);
        scope.setAutomaticLocal(kSlotNew, neu);
        if (a->setAttributeIfEqual(&scope, cc->valueKey, old, neu)) {
            fireWatches(&scope, cc, a, old, neu);
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
        const proto::ProtoArguments* args,
        const proto::ProtoKeywordArguments*) {
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
    setDebugThreadKind("future");

    // The spawning thread's ActiveCallContext, copied for this worker at
    // spawn time (buildFuture), which this worker owns. Not the spawner's
    // thread-local itself: the spawner keeps running and rewrites it (a
    // ThreadSanitizer-reported race before the copy).
    std::unique_ptr<const ActiveCallContext> parentCc(
        reinterpret_cast<const ActiveCallContext*>(blobObj->asLong(ctx)));
    if (!parentCc) return PROTO_NONE;

    // Install the parent's ActiveCallContext on this thread's TLS so
    // engine->invoke can dispatch through the same shared
    // ExecutionEngine + same globals + markers as the spawning thread.
    setActiveCallContext(*parentCc);

    const proto::ProtoObject* value = PROTO_NONE;
    try {
        value = parentCc->engine->invoke(ctx, thunk, nullptr, 0);
    } catch (...) {
        // Recorded on the future, before `__done__`, so a deref that sees
        // the future realised finds it; deref raises it (prim_deref).
        recordThreadError(ctx, fut);
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
    // A copy the worker owns: this thread's context is thread-local and
    // changes as this thread runs on (futureThreadMain).
    auto ccCopy = std::make_unique<ActiveCallContext>(*cc);
    fut->setAttribute(ctx, cc->ccBlobKey,
        ctx->fromLong(reinterpret_cast<long long>(ccCopy.get())));
    fut->setAttribute(ctx, cc->doneKey, PROTO_FALSE);
    fut->setAttribute(ctx, cc->resultKey, PROTO_NONE);

    const proto::ProtoString* threadName =
        proto::ProtoString::createSymbol(ctx, "protoclj-future");
    const proto::ProtoList* targs =
        ctx->newList()->appendLast(ctx, fut);
    const proto::ProtoThread* thread =
        ctx->space->newThread(ctx, threadName, &futureThreadMain,
                              targs, nullptr);
    (void)ccCopy.release();  // the worker owns it from here
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
    {
        // A joining thread still counts in protoCore's runningThreads, so it never
        // parks: the stop-the-world quorum can never be met, no collection cycle can
        // start, and the thread being joined waits for memory that a cycle would have
        // freed. Leaving the running set for the duration of the block is what makes
        // this a join and not a deadlock. Pinned by tests/cli/blocking-joins-park-for-gc.sh.
        // Kept although protoCore's ProtoThread::join now brackets itself
        // (2026-09-25, protoCore >= 2.3).  Nesting is provably idempotent:
        // unmanagedDepth is a counter and only the outermost pair moves
        // parkedThreads, so this guard costs one atomic increment.  It is kept
        // because it protects against a version skew protoClojure's build CANNOT
        // detect -- the build checks SOVERSION (3) and the fix changed no ABI, so a
        // protoClojure linked against protoCore 2.2.0 would deadlock here with no
        // warning.  Verified by experiment: with all four guards removed,
        // tests/cli/blocking-joins-park-for-gc.sh passes in 8.3 s against a fixed
        // kernel and is killed at 90 s against protoCore 9e85a4c0.  Remove these
        // four guards once every embedder requires protoCore 2.3.
        proto::ProtoContext::UnmanagedScope parked(ctx);
        for (const proto::ProtoThread* t : snapshot) {
            if (t) const_cast<proto::ProtoThread*>(t)->join(ctx);
        }
    }
}

const proto::ProtoObject* prim_make_future(proto::ProtoContext* ctx,
                                           const proto::ProtoObject*,
                                           const proto::ParentLink*,
                                           const proto::ProtoArguments* args,
                                           const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1)
        throwError(ctx, "future: expects 1 thunk");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throwError(ctx, "future: no active VM context");
    return buildFuture(ctx, cc, args->getAt(ctx, 0));
}

const proto::ProtoObject* prim_future_p(proto::ProtoContext* ctx,
                                        const proto::ProtoObject*,
                                        const proto::ParentLink*,
                                        const proto::ProtoArguments* args,
                                        const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1)
        throwError(ctx, "future?: expects 1 arg");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throwError(ctx, "future?: no active VM context");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    if (!v) return PROTO_FALSE;
    return (v->getPrototype(ctx) == cc->futureMarkerProto) ? PROTO_TRUE : PROTO_FALSE;
}

const proto::ProtoObject* prim_realized_p(proto::ProtoContext* ctx,
                                          const proto::ProtoObject*,
                                          const proto::ParentLink*,
                                          const proto::ProtoArguments* args,
                                          const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1)
        throwError(ctx, "realized?: expects 1 arg");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throwError(ctx, "realized?: no active VM context");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    if (!v) return PROTO_FALSE;
    // A lazy seq: its body has run (LazySeq.h); a cons or a chunk is a seq
    // already computed.
    switch (seqObjectKind(ctx, v)) {
        case SeqObject::Lazy:  return lazyIsRealized(ctx, v) ? PROTO_TRUE : PROTO_FALSE;
        case SeqObject::Cons:
        case SeqObject::Chunk: return PROTO_TRUE;
        case SeqObject::None:  break;
    }
    const proto::ProtoObject* proto = v->getPrototype(ctx);
    if (proto == cc->promiseMarkerProto) {
        bool delivered = false;
        promiseValue(ctx, cc->valueKey, v, &delivered);
        return delivered ? PROTO_TRUE : PROTO_FALSE;
    }
    if (proto != cc->futureMarkerProto) return PROTO_FALSE;
    // A future's worker writes __result__ (and __error__) before __done__.
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
        const proto::ProtoArguments* args,
        const proto::ProtoKeywordArguments*) {
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
    setDebugThreadKind("pmap");
    const ActiveCallContext* parentCc =
        reinterpret_cast<const ActiveCallContext*>(blobObj->asLong(ctx));
    if (!parentCc) return PROTO_NONE;

    setActiveCallContext(*parentCc);

    const proto::ProtoObject* value = PROTO_NONE;
    try {
        const proto::ProtoObject* one[1] = { x };
        value = parentCc->engine->invoke(ctx, f, one, 1);
    } catch (...) {
        // Recorded on the element; prim_pmap raises it.
        recordThreadError(ctx, fut);
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
                                    const proto::ProtoArguments* args,
                                    const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 2)
        throwError(ctx, "pmap: expects (pmap f coll)");
    const proto::ProtoObject* f    = args->getAt(ctx, 0);
    const proto::ProtoObject* coll = args->getAt(ctx, 1);
    const proto::ProtoList* lst = asSeqOrNull(ctx, coll);
    if (!lst) return ctx->newList()->asObject(ctx);
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throwError(ctx, "pmap: no active VM context");
    proto::proto_ulong n = lst->getSize(ctx);

    const proto::ProtoString* fKey =
        proto::ProtoString::createSymbol(ctx, "__pmap_f__");
    const proto::ProtoString* xKey =
        proto::ProtoString::createSymbol(ctx, "__pmap_x__");
    const proto::ProtoString* threadName =
        proto::ProtoString::createSymbol(ctx, "protoclj-pmap");

    // Phase 1 — spawn all N workers. Collect the future wrappers
    // into a transient ProtoList so the GC can trace them. Both that list and
    // the result list below have one entry per element of the collection, so
    // each is built in chunks; each builder is closed before the next phase
    // starts, because while one is open it must stay the innermost context on
    // this thread (see ListBuilder.h).
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(2);
    constexpr unsigned int kSlotFs  = 0;
    constexpr unsigned int kSlotOut = 1;
    {
        ListBuilder futures(&scope);
        proto::ProtoContext* fctx = futures.context();
        for (proto::proto_ulong i = 0; i < n; ++i) {
            const proto::ProtoObject* x = lst->getAt(fctx, (int)i);
            proto::ProtoObject* fut = const_cast<proto::ProtoObject*>(
                cc->futureMarkerProto->newChild(fctx, /*isMutable=*/true));
            fut->setAttribute(fctx, fKey, f);
            fut->setAttribute(fctx, xKey, x);
            fut->setAttribute(fctx, cc->ccBlobKey,
                fctx->fromLong(reinterpret_cast<long long>(cc)));
            fut->setAttribute(fctx, cc->doneKey, PROTO_FALSE);
            fut->setAttribute(fctx, cc->resultKey, PROTO_NONE);

            const proto::ProtoList* targs =
                fctx->newList()->appendLast(fctx, fut);
            const proto::ProtoThread* t =
                fctx->space->newThread(fctx, threadName, &pmapWorkerMain,
                                       targs, nullptr);
            // The thread object itself, not its address as an integer: the
            // element keeps it reachable until phase 2 has joined it. A
            // finished worker leaves protoCore's thread list, so a thread
            // known only by its address could be collected before the join.
            fut->setAttribute(fctx, cc->threadKey,
                reinterpret_cast<const proto::ProtoObject*>(t));
            futures.push(fut);
        }
        scope.setAutomaticLocal(kSlotFs, futures.finish());
    }

    // Phase 2 — join in order, collect results. An element whose call raised
    // an error makes pmap raise it, wrapped as ExecutionException, as
    // consuming that element of JVM Clojure's pmap does: the first failing
    // element in input order is the one raised, after the loop has waited
    // for every element.
    const proto::ProtoList* fs =
        scope.getAutomaticLocal(kSlotFs)->asList(&scope);
    proto::proto_long firstFailed = -1;   // index of the first failing element
    {
        ListBuilder results(&scope);
        proto::ProtoContext* rctx = results.context();
        for (proto::proto_ulong i = 0; i < n; ++i) {
            const proto::ProtoObject* fut = fs->getAt(rctx, (int)i);
            // Every worker is joined, finished or not: a worker that has
            // already finished still holds its native thread (its stack and
            // its std::thread) until it is joined, so skipping the join for
            // done elements leaked one thread per element
            // (tests/cli/pmap-joins-workers.sh). Joining a finished thread
            // returns at once. These threads are pmap's own: nothing else
            // joins them.
            {
                const proto::ProtoObject* tObj =
                    fut->getAttribute(rctx, cc->threadKey);
                if (tObj && tObj != PROTO_NONE) {
                    proto::ProtoThread* t =
                        const_cast<proto::ProtoThread*>(tObj->asThread(rctx));
                    if (t) {
                        // See the note at shutdownFuturesImpl. pmap joins in
                        // order, so without this the first element that needs
                        // a cycle deadlocks the whole call.
                        // Kept although protoCore's ProtoThread::join now brackets itself
                        // (2026-09-25, protoCore >= 2.3).  Nesting is provably idempotent:
                        // unmanagedDepth is a counter and only the outermost pair moves
                        // parkedThreads, so this guard costs one atomic increment.  It is kept
                        // because it protects against a version skew protoClojure's build CANNOT
                        // detect -- the build checks SOVERSION (3) and the fix changed no ABI, so a
                        // protoClojure linked against protoCore 2.2.0 would deadlock here with no
                        // warning.  Verified by experiment: with all four guards removed,
                        // tests/cli/blocking-joins-park-for-gc.sh passes in 8.3 s against a fixed
                        // kernel and is killed at 90 s against protoCore 9e85a4c0.  Remove these
                        // four guards once every embedder requires protoCore 2.3.
                        proto::ProtoContext::UnmanagedScope parked(rctx);
                        t->join(rctx);
                    }
                }
            }
            if (firstFailed < 0 && threadFailure(rctx, fut))
                firstFailed = static_cast<proto::proto_long>(i);
            results.push(fut->getAttribute(rctx, cc->resultKey));
        }
        scope.setAutomaticLocal(kSlotOut, results.finish());
    }
    if (firstFailed >= 0) {
        // The element stays rooted in the kSlotFs list, and its exception
        // with it.
        throwExecutionException(&scope, threadFailure(&scope,
            fs->getAt(&scope, static_cast<int>(firstFailed))));
    }
    return scope.getAutomaticLocal(kSlotOut);
}

// Session 18 — promises. A promise is a child of promiseMarkerProto
// with `__value__` initially nil and `__done__` PROTO_FALSE. deliver
// CAS-installs the value once; subsequent delivers are no-ops and
// return nil. deref busy-waits on __done__ — see prim_deref above.

const proto::ProtoObject* prim_promise(proto::ProtoContext* ctx,
                                       const proto::ProtoObject*,
                                       const proto::ParentLink*,
                                       const proto::ProtoArguments* args,
                                       const proto::ProtoKeywordArguments*) {
    if (args && args->getSize(ctx) != 0)
        throwError(ctx, "promise: takes no arguments");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throwError(ctx, "promise: no active VM context");
    // A pending promise has no own value attribute: deliverPromise installs
    // it with one compare-and-set, which is why the promise is mutable.
    const proto::ProtoObject* p =
        cc->promiseMarkerProto->newChild(ctx, /*isMutable=*/true);
    return p;
}

const proto::ProtoObject* prim_promise_p(proto::ProtoContext* ctx,
                                         const proto::ProtoObject*,
                                         const proto::ParentLink*,
                                         const proto::ProtoArguments* args,
                                         const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1)
        throwError(ctx, "promise?: expects 1 arg");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throwError(ctx, "promise?: no active VM context");
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
        throwError(ctx, std::string(primName) + ": not an actor");
    const proto::ProtoObject* h = v->getAttribute(ctx, cc->actorStateKey);
    if (!h || !h->isInteger(ctx))
        throwError(ctx, std::string(primName) + ": broken actor handle");
    return reinterpret_cast<ActorState*>(h->asLong(ctx));
}

const proto::ProtoObject* prim_actor(proto::ProtoContext* ctx,
                                     const proto::ProtoObject*,
                                     const proto::ParentLink*,
                                     const proto::ProtoArguments* args,
                                     const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1)
        throwError(ctx, "actor: expects (actor initial-state)");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throwError(ctx, "actor: no active VM context");

    auto& sched = ActorScheduler::instance();
    sched.ensureStarted(ctx->space, ctx, *cc);

    const proto::ProtoObject* init = args->getAt(ctx, 0);
    proto::ProtoObject* wrap = const_cast<proto::ProtoObject*>(
        cc->actorMarkerProto->newChild(ctx, /*isMutable=*/true));
    wrap->setAttribute(ctx, cc->valueKey, init);

    // The three mailboxes (PMQ-SPEC §2.1: one queue per priority band).
    // They go on the wrapper BEFORE the ActorState exists: the wrapper is
    // mutable and therefore a GC root, and that attribute is the only thing
    // that keeps the queues — and every message in them, with its function,
    // its arguments and its promise — reachable by the collector.
    const proto::ProtoMPSCQueue* high   = ctx->newMPSCQueue();
    const proto::ProtoMPSCQueue* medium = ctx->newMPSCQueue();
    const proto::ProtoMPSCQueue* low    = ctx->newMPSCQueue();
    const proto::ProtoObject* mailboxes[3] = {
        high->asObject(ctx), medium->asObject(ctx), low->asObject(ctx)};
    wrap->setAttribute(ctx, cc->mailboxKey,
                       ctx->newList(3, mailboxes)->asObject(ctx));

    ActorState* state = sched.newActor(wrap, init, high, medium, low);
    wrap->setAttribute(ctx, cc->actorStateKey,
        ctx->fromLong(reinterpret_cast<long long>(state)));
    return wrap;
}

const proto::ProtoObject* prim_actor_p(proto::ProtoContext* ctx,
                                       const proto::ProtoObject*,
                                       const proto::ParentLink*,
                                       const proto::ProtoArguments* args,
                                       const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1)
        throwError(ctx, "actor?: expects 1 arg");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throwError(ctx, "actor?: no active VM context");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    if (!v) return PROTO_FALSE;
    return (v->getPrototype(ctx) == cc->actorMarkerProto)
           ? PROTO_TRUE : PROTO_FALSE;
}

// Shared core for send / send-h / send-m / send-l.
static const proto::ProtoObject* sendCore(proto::ProtoContext* ctx,
                                           const proto::ProtoArguments* args,
                                           const char* primName,
                                           ActorPriority priority) {
    if (!args || args->getSize(ctx) < 2)
        throwError(ctx, std::string(primName) + ": expects (… actor f & args)");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throwError(ctx, std::string(primName) + ": no active VM context");
    ActorState* state = actorStateFrom(ctx, cc, args->getAt(ctx, 0), primName);
    const proto::ProtoObject* f = args->getAt(ctx, 1);

    // Build the per-send promise — same wire shape as `promise`.
    // A pending promise has no own value attribute: deliverPromise installs
    // it with one compare-and-set, which is why the promise is mutable.
    // Being mutable also makes it a GC root, so it survives the scope below.
    const proto::ProtoObject* p =
        cc->promiseMarkerProto->newChild(ctx, /*isMutable=*/true);

    // The extra arguments, the message list and the queue node it is pushed
    // into are all allocated in a scope that ends here, so protoCore sees
    // them as candidates as soon as the send returns. Through `ctx` they
    // would stay in the young generation of the caller's frame, which in a
    // `(loop … (send a f))` is one context for every message the loop sends.
    {
        proto::ProtoContext scope(ctx->space, ctx);
        // Pack the extra args (positions 2..N-1) into a fresh ProtoList.
        // Their number is the call's arity, which `apply` can make
        // arbitrarily large, so the pack is chunked; the builder is closed
        // before anything else on this thread pushes a context
        // (see ListBuilder.h). A send with no extra arguments — the common
        // case, and the one the actor benchmarks measure — skips it.
        const proto::ProtoObject* extras = PROTO_NONE;
        const proto::proto_ulong n = args->getSize(&scope);
        if (n > 2) {
            ListBuilder b(&scope);
            for (proto::proto_ulong i = 2; i < n; ++i) {
                b.push(args->getAt(b.context(), static_cast<int>(i)));
            }
            extras = b.finish();
        }
        ActorScheduler::instance().send(
            &scope, state, priority, newMessage(&scope, f, extras, p));
    }
    return p;
}

const proto::ProtoObject* prim_send(proto::ProtoContext* ctx,
                                    const proto::ProtoObject*,
                                    const proto::ParentLink*,
                                    const proto::ProtoArguments* args,
                                    const proto::ProtoKeywordArguments*) {
    return sendCore(ctx, args, "send", ActorPriority::Medium);
}

const proto::ProtoObject* prim_send_h(proto::ProtoContext* ctx,
                                      const proto::ProtoObject*,
                                      const proto::ParentLink*,
                                      const proto::ProtoArguments* args,
                                      const proto::ProtoKeywordArguments*) {
    return sendCore(ctx, args, "send-h", ActorPriority::High);
}

const proto::ProtoObject* prim_send_m(proto::ProtoContext* ctx,
                                      const proto::ProtoObject*,
                                      const proto::ParentLink*,
                                      const proto::ProtoArguments* args,
                                      const proto::ProtoKeywordArguments*) {
    return sendCore(ctx, args, "send-m", ActorPriority::Medium);
}

const proto::ProtoObject* prim_send_l(proto::ProtoContext* ctx,
                                      const proto::ProtoObject*,
                                      const proto::ParentLink*,
                                      const proto::ProtoArguments* args,
                                      const proto::ProtoKeywordArguments*) {
    return sendCore(ctx, args, "send-l", ActorPriority::Low);
}

// (actor-stats) — diagnostics, returns a small map.
const proto::ProtoObject* prim_actor_stats(proto::ProtoContext* ctx,
                                           const proto::ProtoObject*,
                                           const proto::ParentLink*,
                                           const proto::ProtoArguments*,
                                           const proto::ProtoKeywordArguments*) {
    auto s = ActorScheduler::instance().stats();
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throwError(ctx, "actor-stats: no active VM context");
    // Counts are SmallInts and keywords are interned (reachable through the
    // intern table, Named.h): nothing here needs rooting before the map is
    // built.
    const proto::ProtoObject* kv[4] = {
        internNamed(ctx, cc->named, ":workers"),
        ctx->fromLong(s.numWorkers),
        internNamed(ctx, cc->named, ":messages-processed"),
        ctx->fromLong(static_cast<long long>(s.messagesProcessed))};
    return mapAssocPairs(ctx, nullptr, kv, 4);
}

const proto::ProtoObject* prim_deliver(proto::ProtoContext* ctx,
                                       const proto::ProtoObject*,
                                       const proto::ParentLink*,
                                       const proto::ProtoArguments* args,
                                       const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 2)
        throwError(ctx, "deliver: expects (deliver promise value)");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throwError(ctx, "deliver: no active VM context");
    const proto::ProtoObject* p = args->getAt(ctx, 0);
    if (!p || p->getPrototype(ctx) != cc->promiseMarkerProto)
        throwError(ctx, "deliver: not a promise");
    const proto::ProtoObject* v = args->getAt(ctx, 1);
    // Single-shot: the first delivery wins and returns the promise; later
    // ones change nothing and return nil.
    return deliverPromise(ctx, cc->valueKey, p, v) ? p : PROTO_NONE;
}

const proto::ProtoObject* prim_compare_and_set_bang(
        proto::ProtoContext* ctx,
        const proto::ProtoObject*,
        const proto::ParentLink*,
        const proto::ProtoArguments* args,
        const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 3)
        throwError(ctx, "compare-and-set!: expects (compare-and-set! atom old new)");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throwError(ctx, "compare-and-set!: no active VM context");
    const proto::ProtoObject* a = args->getAt(ctx, 0);
    if (!a || a->getPrototype(ctx) != cc->atomMarkerProto)
        throwError(ctx, "compare-and-set!: not an atom");
    const proto::ProtoObject* expected = args->getAt(ctx, 1);
    const proto::ProtoObject* nv       = args->getAt(ctx, 2);
    bool ok = a->setAttributeIfEqual(ctx, cc->valueKey, expected, nv);
    if (ok) fireWatches(ctx, cc, a, expected, nv);
    return ok ? PROTO_TRUE : PROTO_FALSE;
}

const proto::ProtoObject* prim_add_watch(proto::ProtoContext* ctx,
                                         const proto::ProtoObject*,
                                         const proto::ParentLink*,
                                         const proto::ProtoArguments* args,
                                         const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 3)
        throwError(ctx, "add-watch: expects (add-watch atom key fn)");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throwError(ctx, "add-watch: no active VM context");
    const proto::ProtoObject* a = args->getAt(ctx, 0);
    if (!a || a->getPrototype(ctx) != cc->atomMarkerProto)
        throwError(ctx, "add-watch: not an atom");
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
            isMap(old) ? old : nullptr;
        scope.setAutomaticLocal(0,
            mapAssocPairs(&scope, base, kf, 2));
        if (a->setAttributeIfEqual(&scope, cc->watchesKey, old,
                                   scope.getAutomaticLocal(0))) break;
    }
    return a;
}

const proto::ProtoObject* prim_remove_watch(proto::ProtoContext* ctx,
                                            const proto::ProtoObject*,
                                            const proto::ParentLink*,
                                            const proto::ProtoArguments* args,
                                            const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 2)
        throwError(ctx, "remove-watch: expects (remove-watch atom key)");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throwError(ctx, "remove-watch: no active VM context");
    const proto::ProtoObject* a = args->getAt(ctx, 0);
    if (!a || a->getPrototype(ctx) != cc->atomMarkerProto)
        throwError(ctx, "remove-watch: not an atom");
    const proto::ProtoObject* k = args->getAt(ctx, 1);

    // CAS the watches map without `k`; `neu` is rooted across the CAS, which
    // allocates.
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(1);
    for (;;) {
        const proto::ProtoObject* old =
            a->getOwnAttributeDirect(&scope, cc->watchesKey);
        if (!isMap(old)) return a;
        scope.setAutomaticLocal(0, mapDissoc(&scope, old, k));
        if (scope.getAutomaticLocal(0) == old) return a;   // key absent
        if (a->setAttributeIfEqual(&scope, cc->watchesKey, old,
                                   scope.getAutomaticLocal(0))) break;
    }
    return a;
}

// (import-module "logical/path"): the module object protoCore's Unified
// Module Discovery resolves for the path through the space's resolution
// chain (ProtoSpace::getImportModule), cached process-wide. Its members are
// read as `alias/member` and called as `(alias/member args...)`, with the
// module as the receiver; its error types are caught as
// `(catch alias/ErrorType e ...)` (docs/INTEROP.md).
const proto::ProtoObject* prim_import_module(proto::ProtoContext* ctx,
                                             const proto::ProtoObject*,
                                             const proto::ParentLink*,
                                             const proto::ProtoArguments* args,
                                             const proto::ProtoKeywordArguments*) {
    const proto::proto_ulong n = args ? args->getSize(ctx) : 0;
    if (n != 1)
        throwClassed(ctx, "ArityException",
            "Wrong number of args (" + std::to_string(n) + ") passed to: import-module");
    const proto::ProtoObject* path = args->getAt(ctx, 0);
    if (!path || path == PROTO_NONE || !proto::ProtoObject::isStringTagFast(path))
        throwClassed(ctx, "ClassCastException",
            std::string("import-module expects a string path, got ") + valueTypeName(ctx, path));
    const std::string logical =
        reinterpret_cast<const proto::ProtoString*>(path)->toStdString(ctx);
    const proto::ProtoObject* wrapper =
        ctx->space->getImportModule(ctx, logical.c_str(), "exports");
    const proto::ProtoObject* module = (wrapper && wrapper != PROTO_NONE)
        ? wrapper->getAttribute(ctx, proto::ProtoString::createSymbol(ctx, "exports"))
        : nullptr;
    if (!module || module == PROTO_NONE)
        throwClassed(ctx, "FileNotFoundException",
            "Could not locate module " + logical + " on the resolution chain");
    return module;
}

// Macros (Macros.h) and eval --------------------------------------------
// ArityException "Wrong number of args (n) passed to: name", as JVM Clojure
// reports a built-in called with the wrong number of arguments.
[[noreturn, gnu::cold]]
void arityError(proto::ProtoContext* ctx, const proto::ProtoArguments* args, const char* name) {
    const proto::proto_ulong n = args ? args->getSize(ctx) : 0;
    throwClassed(ctx, "ArityException",
                 "Wrong number of args (" + std::to_string(n) + ") passed to: " + name);
}


// (__macro__ f) — `f` with the macro flag set: what `defmacro` stores. A fn
// wrapper is immutable, so setAttribute answers a new wrapper with the same
// prototype and attributes plus the flag.
const proto::ProtoObject* prim_make_macro(proto::ProtoContext* ctx,
                                          const proto::ProtoObject*,
                                          const proto::ParentLink*,
                                          const proto::ProtoArguments* args,
                                          const proto::ProtoKeywordArguments*) {
    const ActiveCallContext* cc = activeCallContext();
    const proto::ProtoObject* f = (args && args->getSize(ctx) == 1) ? args->getAt(ctx, 0) : nullptr;
    if (!cc || !f || (f->getPrototype(ctx) != cc->fnSingleProto &&
                      f->getPrototype(ctx) != cc->fnMultiProto))
        throwError(ctx, "IllegalArgumentException: defmacro expects a fn");
    return f->setAttribute(ctx, macroFlagKey(ctx), PROTO_TRUE);
}

// (macroexpand-1 form) — one step of expansion with no locals in scope; a
// form that is not a macro call is returned unchanged.
const proto::ProtoObject* prim_macroexpand_1(proto::ProtoContext* ctx,
                                             const proto::ProtoObject*,
                                             const proto::ParentLink*,
                                             const proto::ProtoArguments* args,
                                             const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1) arityError(ctx, args, "macroexpand-1");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throwError(ctx, "macroexpand-1: no active VM context");
    return macroexpandOnce(ctx, *cc, args->getAt(ctx, 0));
}

// (macroexpand form) — macroexpand-1 repeated until the form is no longer a
// macro call. Each step's result is rooted in a slot of its own scope.
const proto::ProtoObject* prim_macroexpand(proto::ProtoContext* ctx,
                                           const proto::ProtoObject*,
                                           const proto::ParentLink*,
                                           const proto::ProtoArguments* args,
                                           const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1) arityError(ctx, args, "macroexpand");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throwError(ctx, "macroexpand: no active VM context");
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(1);
    scope.setAutomaticLocal(0, args->getAt(ctx, 0));
    for (;;) {
        checkNativeStack(StackUse::Source);
        const proto::ProtoObject* f = scope.getAutomaticLocal(0);
        const proto::ProtoObject* e = macroexpandOnce(&scope, *cc, f);
        if (e == f) break;
        scope.setAutomaticLocal(0, e);
    }
    scope.returnValue = scope.getAutomaticLocal(0);
    return scope.getAutomaticLocal(0);
}

// (eval form) — compiles and runs `form` as a top-level form (Eval.h).
const proto::ProtoObject* prim_eval(proto::ProtoContext* ctx,
                                    const proto::ProtoObject*,
                                    const proto::ParentLink*,
                                    const proto::ProtoArguments* args,
                                    const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1) arityError(ctx, args, "eval");
    EvalHook hook = evalHook();
    if (!hook) throwError(ctx, "UnsupportedOperationException: eval is not available");
    return hook(ctx, args->getAt(ctx, 0));
}

// Symbols ------------------------------------------------------------------

// The spelling of a symbol or keyword `v` (with a keyword's `:`), or of a
// string; false for any other value.
bool spellingOf(proto::ProtoContext* ctx, const ActiveCallContext* cc,
                const proto::ProtoObject* v, std::string& out) {
    if (cc && isNamed(ctx, cc->named, v)) {
        out = namedSpelling(ctx, cc->named, v)->toStdString(ctx);
        return true;
    }
    if (v && proto::ProtoObject::isStringTagFast(v)) {
        out = reinterpret_cast<const proto::ProtoString*>(v)->toStdString(ctx);
        return true;
    }
    return false;
}

// (gensym) / (gensym prefix) — a fresh symbol, `G__N` or `prefixN`, N from
// the process-wide sequence (Named.h), as in Clojure.
const proto::ProtoObject* prim_gensym(proto::ProtoContext* ctx,
                                      const proto::ProtoObject*,
                                      const proto::ParentLink*,
                                      const proto::ProtoArguments* args,
                                      const proto::ProtoKeywordArguments*) {
    const proto::proto_ulong n = args ? args->getSize(ctx) : 0;
    if (n > 1) arityError(ctx, args, "gensym");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throwError(ctx, "gensym: no active VM context");
    std::string prefix = "G__";
    if (n == 1 && !spellingOf(ctx, cc, args->getAt(ctx, 0), prefix))
        throwError(ctx, "ClassCastException: gensym expects a string prefix, got " +
                        std::string(valueTypeName(ctx, args->getAt(ctx, 0))));
    if (n == 1 && !prefix.empty() && prefix[0] == ':') prefix.erase(0, 1);
    const std::string spelling = prefix + std::to_string(nextGensymId());
    return internNamed(ctx, cc->named, spelling.c_str());
}

// (symbol name) / (symbol ns name) — the symbol of a string (or the symbol
// itself; a keyword loses its `:`), and `ns/name` with two arguments.
const proto::ProtoObject* prim_symbol(proto::ProtoContext* ctx,
                                      const proto::ProtoObject*,
                                      const proto::ParentLink*,
                                      const proto::ProtoArguments* args,
                                      const proto::ProtoKeywordArguments*) {
    const proto::proto_ulong n = args ? args->getSize(ctx) : 0;
    if (n != 1 && n != 2) arityError(ctx, args, "symbol");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throwError(ctx, "symbol: no active VM context");
    std::string spelling;
    for (proto::proto_ulong i = 0; i < n; ++i) {
        const proto::ProtoObject* a = args->getAt(ctx, static_cast<int>(i));
        std::string part;
        if (n == 2 && i == 0 && (!a || a == PROTO_NONE)) continue;  // (symbol nil "x")
        if (!spellingOf(ctx, cc, a, part))
            throwError(ctx, "ClassCastException: symbol expects a string, got " +
                            std::string(valueTypeName(ctx, a)));
        if (!part.empty() && part[0] == ':') part.erase(0, 1);
        if (n == 2 && i == 0) part += '/';
        spelling += part;
    }
    if (spelling.empty() || spelling == "nil" || spelling == "true" || spelling == "false")
        throwError(ctx, "IllegalArgumentException: symbol: not a symbol name: \"" + spelling + "\"");
    return internNamed(ctx, cc->named, spelling.c_str());
}

// (keyword name) / (keyword ns name) — the keyword of a string or symbol
// (a keyword is itself); nil for nil.
const proto::ProtoObject* prim_keyword(proto::ProtoContext* ctx,
                                       const proto::ProtoObject*,
                                       const proto::ParentLink*,
                                       const proto::ProtoArguments* args,
                                       const proto::ProtoKeywordArguments*) {
    const proto::proto_ulong n = args ? args->getSize(ctx) : 0;
    if (n != 1 && n != 2) arityError(ctx, args, "keyword");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throwError(ctx, "keyword: no active VM context");
    if (n == 1 && (!args->getAt(ctx, 0) || args->getAt(ctx, 0) == PROTO_NONE)) return PROTO_NONE;
    std::string spelling = ":";
    for (proto::proto_ulong i = 0; i < n; ++i) {
        const proto::ProtoObject* a = args->getAt(ctx, static_cast<int>(i));
        std::string part;
        if (n == 2 && i == 0 && (!a || a == PROTO_NONE)) continue;
        if (!spellingOf(ctx, cc, a, part))
            throwError(ctx, "ClassCastException: keyword expects a string, got " +
                            std::string(valueTypeName(ctx, a)));
        if (!part.empty() && part[0] == ':') part.erase(0, 1);
        if (n == 2 && i == 0) part += '/';
        spelling += part;
    }
    return internNamed(ctx, cc->named, spelling.c_str());
}

// (name x) — the name of a symbol or keyword without its namespace (and a
// keyword without its `:`); a string is its own name.
const proto::ProtoObject* prim_name(proto::ProtoContext* ctx,
                                    const proto::ProtoObject*,
                                    const proto::ParentLink*,
                                    const proto::ProtoArguments* args,
                                    const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1) arityError(ctx, args, "name");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    if (v && proto::ProtoObject::isStringTagFast(v)) return v;
    const ActiveCallContext* cc = activeCallContext();
    std::string s;
    if (!cc || !isNamed(ctx, cc->named, v) || !spellingOf(ctx, cc, v, s))
        throwError(ctx, "ClassCastException: name expects a symbol, keyword or string, got " +
                        std::string(valueTypeName(ctx, v)));
    if (!s.empty() && s[0] == ':') s.erase(0, 1);
    const std::size_t slash = s.find('/');
    if (slash != std::string::npos && slash + 1 < s.size()) s = s.substr(slash + 1);
    return makeUtf8String(ctx, s);
}

// (list* args... coll) — a list of the leading arguments followed by the
// elements of the last one (a list, vector, map, string or nil); nil when
// there are none, as in Clojure. (list* nil) is nil.
const proto::ProtoObject* prim_list_star(proto::ProtoContext* ctx,
                                         const proto::ProtoObject*,
                                         const proto::ParentLink*,
                                         const proto::ProtoArguments* args,
                                         const proto::ProtoKeywordArguments*) {
    const proto::proto_ulong n = args ? args->getSize(ctx) : 0;
    if (n == 0) arityError(ctx, args, "list*");
    if (isSeqObject(ctx, args->getAt(ctx, static_cast<int>(n - 1)))) {
        // (list* x y lazy): cons cells in front of the lazy tail, which stays
        // unrealised, as in Clojure.
        proto::ProtoContext scope(ctx->space, ctx);
        scope.resizeAutomaticLocals(1);
        scope.setAutomaticLocal(0, args->getAt(&scope, static_cast<int>(n - 1)));
        for (proto::proto_ulong i = n - 1; i-- > 0;)
            scope.setAutomaticLocal(0, newCons(&scope, args->getAt(&scope, static_cast<int>(i)),
                                               scope.getAutomaticLocal(0)));
        const proto::ProtoObject* r = scope.getAutomaticLocal(0);
        scope.returnValue = r;
        return r;
    }
    ListBuilder out(ctx);
    for (proto::proto_ulong i = 0; i + 1 < n; ++i)
        out.push(args->getAt(out.context(), static_cast<int>(i)));
    const proto::ProtoObject* tail = args->getAt(out.context(), static_cast<int>(n - 1));
    if (!pushSeqElements(out, tail))
        throwError(out.context(), "Don't know how to create ISeq from: " +
                                  printedValue(out.context(), tail));
    if (out.size() == 0) return PROTO_NONE;
    return out.finish();
}

// (even? n) / (odd? n) — integers only, of any size, as in Clojure.
bool integerIsEven(proto::ProtoContext* ctx, const proto::ProtoArguments* args, const char* name) {
    if (!args || args->getSize(ctx) != 1) arityError(ctx, args, name);
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    if (!v || !v->isInteger(ctx))
        throwClassed(ctx, "IllegalArgumentException",
                     "Argument must be an integer: " + printedValue(ctx, v));
    const proto::ProtoObject* r = v->modulo(ctx, ctx->fromLong(2));
    return r->isInteger(ctx) && r->asLong(ctx) == 0;
}

const proto::ProtoObject* prim_even_p(proto::ProtoContext* ctx,
                                      const proto::ProtoObject*,
                                      const proto::ParentLink*,
                                      const proto::ProtoArguments* args,
                                      const proto::ProtoKeywordArguments*) {
    return integerIsEven(ctx, args, "even?") ? PROTO_TRUE : PROTO_FALSE;
}

const proto::ProtoObject* prim_odd_p(proto::ProtoContext* ctx,
                                     const proto::ProtoObject*,
                                     const proto::ParentLink*,
                                     const proto::ProtoArguments* args,
                                     const proto::ProtoKeywordArguments*) {
    return integerIsEven(ctx, args, "odd?") ? PROTO_FALSE : PROTO_TRUE;
}

// (__raise-illegal-argument message) — raises IllegalArgumentException with
// `message`. The core.clj macros use it where Clojure's throw an
// IllegalArgumentException (a binding vector of the wrong shape, a `case`
// with no matching clause); there is no Java constructor to call.
const proto::ProtoObject* prim_raise_illegal_argument(proto::ProtoContext* ctx,
                                                      const proto::ProtoObject*,
                                                      const proto::ParentLink*,
                                                      const proto::ProtoArguments* args,
                                                      const proto::ProtoKeywordArguments*) {
    std::string message;
    if (args && args->getSize(ctx) > 0) appendStr(ctx, message, args->getAt(ctx, 0));
    throwClassed(ctx, "IllegalArgumentException", message);
}

// (__destructure-map v) — what a map binding form destructures, as
// clojure.core/destructure computes it: a seq (here, a list) of alternating
// keys and values becomes the map they make (`[& {:as opts}]` called with
// `:a 1`), a one-element seq its element (a trailing map, Clojure 1.11), an
// empty seq the empty map; any other value is itself.
const proto::ProtoObject* prim_destructure_map(proto::ProtoContext* ctx,
                                               const proto::ProtoObject*,
                                               const proto::ParentLink*,
                                               const proto::ProtoArguments* args,
                                               const proto::ProtoKeywordArguments*) {
    const proto::ProtoObject* v = (args && args->getSize(ctx) == 1) ? args->getAt(ctx, 0) : PROTO_NONE;
    if (!isSeqValue(ctx, v)) return v;
    const proto::ProtoList* lst = asSeqOrNull(ctx, v);
    const proto::proto_ulong n = lst->getSize(ctx);
    if (n == 1) return lst->getFirst(ctx);
    if (n % 2 != 0)
        throwClassed(ctx, "IllegalArgumentException",
                     "No value supplied for key: " + printedValue(ctx, lst->getAt(ctx, static_cast<int>(n - 1))));
    std::vector<const proto::ProtoObject*> kv;
    kv.reserve(n);
    for (proto::proto_ulong i = 0; i < n; ++i) kv.push_back(lst->getAt(ctx, static_cast<int>(i)));
    return mapAssocPairs(ctx, nullptr, kv.data(), n);
}

// Sets (SetOps.h) ----------------------------------------------------------

// The elements of `coll` (nil, a list, vector, map, set or string) gathered
// into `out`; each is reachable from `coll`, which the caller roots.
void collectElements(proto::ProtoContext* ctx, const proto::ProtoObject* coll,
                     std::vector<const proto::ProtoObject*>& out) {
    if (isSet(ctx, coll)) {
        setForEach(ctx, coll, &out,
            [](proto::ProtoContext*, void* self, const proto::ProtoObject* e) {
                static_cast<std::vector<const proto::ProtoObject*>*>(self)->push_back(e);
            });
        return;
    }
    const proto::ProtoList* lst = asSeqOrNull(ctx, coll);
    if (!lst) return;
    const proto::proto_ulong n = lst->getSize(ctx);
    out.reserve(out.size() + n);
    for (proto::proto_ulong i = 0; i < n; ++i) out.push_back(lst->getAt(ctx, static_cast<int>(i)));
}

// The set argument `i` of `who`: a set, or nil as the empty set.
const proto::ProtoObject* setArg(proto::ProtoContext* ctx, const proto::ProtoArguments* args,
                                 proto::proto_ulong i, const char* who) {
    const proto::ProtoObject* s = args->getAt(ctx, static_cast<int>(i));
    if (!s || s == PROTO_NONE || isSet(ctx, s)) return s;
    throwClassed(ctx, "ClassCastException", std::string(who) + " expects a set, got " +
                                            valueTypeName(ctx, s));
}

// (hash-set x ...) — the set of the arguments; also what `#{...}` compiles to.
const proto::ProtoObject* prim_hash_set(proto::ProtoContext* ctx,
                                        const proto::ProtoObject*,
                                        const proto::ParentLink*,
                                        const proto::ProtoArguments* args,
                                        const proto::ProtoKeywordArguments*) {
    const proto::proto_ulong n = args ? args->getSize(ctx) : 0;
    return setConj(ctx, nullptr, n ? args->values() : nullptr, n);
}

// (set coll) — the set of the elements of a collection (a set is itself).
const proto::ProtoObject* prim_set(proto::ProtoContext* ctx,
                                   const proto::ProtoObject*,
                                   const proto::ParentLink*,
                                   const proto::ProtoArguments* args,
                                   const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1) throwError(ctx, "set: expects 1 arg");
    const proto::ProtoObject* coll = args->getAt(ctx, 0);
    if (isSet(ctx, coll)) return coll;
    std::vector<const proto::ProtoObject*> items;
    collectElements(ctx, coll, items);
    return setConj(ctx, nullptr, items.data(), items.size());
}

const proto::ProtoObject* prim_set_p(proto::ProtoContext* ctx,
                                     const proto::ProtoObject*,
                                     const proto::ParentLink*,
                                     const proto::ProtoArguments* args,
                                     const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1) throwError(ctx, "set?: expects 1 arg");
    return isSet(ctx, args->getAt(ctx, 0)) ? PROTO_TRUE : PROTO_FALSE;
}

// (disj s) / (disj s x & xs) — `s` without the given elements; nil stays nil.
const proto::ProtoObject* prim_disj(proto::ProtoContext* ctx,
                                    const proto::ProtoObject*,
                                    const proto::ParentLink*,
                                    const proto::ProtoArguments* args,
                                    const proto::ProtoKeywordArguments*) {
    const proto::proto_ulong n = args ? args->getSize(ctx) : 0;
    if (n < 1) throwError(ctx, "disj: expects (disj set x ...)");
    const proto::ProtoObject* s = setArg(ctx, args, 0, "disj");
    if (!s || s == PROTO_NONE) return PROTO_NONE;
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(1);
    scope.setAutomaticLocal(0, s);
    for (proto::proto_ulong i = 1; i < n; ++i)
        scope.setAutomaticLocal(0, setDisj(&scope, scope.getAutomaticLocal(0),
                                           args->getAt(&scope, static_cast<int>(i))));
    return scope.getAutomaticLocal(0);
}

// clojure.set/union: every element of every set; (union) is #{}.
const proto::ProtoObject* prim_union(proto::ProtoContext* ctx,
                                     const proto::ProtoObject*,
                                     const proto::ParentLink*,
                                     const proto::ProtoArguments* args,
                                     const proto::ProtoKeywordArguments*) {
    const proto::proto_ulong n = args ? args->getSize(ctx) : 0;
    if (n == 0) return setConj(ctx, nullptr, nullptr, 0);
    const proto::ProtoObject* first = setArg(ctx, args, 0, "union");
    std::vector<const proto::ProtoObject*> items;
    for (proto::proto_ulong i = 1; i < n; ++i) collectElements(ctx, setArg(ctx, args, i, "union"), items);
    if (!first || first == PROTO_NONE) {
        if (n == 1) return PROTO_NONE;
        return setConj(ctx, nullptr, items.data(), items.size());
    }
    return setConj(ctx, first, items.data(), items.size());
}

// clojure.set/intersection: the elements of the first set that every other
// set holds.
const proto::ProtoObject* prim_intersection(proto::ProtoContext* ctx,
                                            const proto::ProtoObject*,
                                            const proto::ParentLink*,
                                            const proto::ProtoArguments* args,
                                            const proto::ProtoKeywordArguments*) {
    const proto::proto_ulong n = args ? args->getSize(ctx) : 0;
    if (n == 0) throwError(ctx, "intersection: expects at least 1 set");
    const proto::ProtoObject* first = setArg(ctx, args, 0, "intersection");
    if (n == 1) return first;
    for (proto::proto_ulong i = 1; i < n; ++i) setArg(ctx, args, i, "intersection");
    std::vector<const proto::ProtoObject*> items, kept;
    collectElements(ctx, first, items);
    for (const proto::ProtoObject* e : items) {
        bool everywhere = true;
        for (proto::proto_ulong i = 1; i < n && everywhere; ++i) {
            bool found = false;
            setGet(ctx, args->getAt(ctx, static_cast<int>(i)), e, &found);
            everywhere = found;
        }
        if (everywhere) kept.push_back(e);
    }
    if (kept.size() == items.size()) return first;
    return setConj(ctx, nullptr, kept.data(), kept.size());
}

// clojure.set/difference: the first set without the elements of the others.
const proto::ProtoObject* prim_difference(proto::ProtoContext* ctx,
                                          const proto::ProtoObject*,
                                          const proto::ParentLink*,
                                          const proto::ProtoArguments* args,
                                          const proto::ProtoKeywordArguments*) {
    const proto::proto_ulong n = args ? args->getSize(ctx) : 0;
    if (n == 0) throwError(ctx, "difference: expects at least 1 set");
    const proto::ProtoObject* first = setArg(ctx, args, 0, "difference");
    for (proto::proto_ulong i = 1; i < n; ++i) setArg(ctx, args, i, "difference");
    if (n == 1 || !first || first == PROTO_NONE) return first;
    std::vector<const proto::ProtoObject*> items, kept;
    collectElements(ctx, first, items);
    for (const proto::ProtoObject* e : items) {
        bool elsewhere = false;
        for (proto::proto_ulong i = 1; i < n && !elsewhere; ++i) {
            bool found = false;
            setGet(ctx, args->getAt(ctx, static_cast<int>(i)), e, &found);
            elsewhere = found;
        }
        if (!elsewhere) kept.push_back(e);
    }
    if (kept.size() == items.size()) return first;
    return setConj(ctx, nullptr, kept.data(), kept.size());
}

// clojure.set/subset? and superset?: every element of the smaller set in the
// larger one.
bool isSubset(proto::ProtoContext* ctx, const proto::ProtoObject* a, const proto::ProtoObject* b) {
    std::vector<const proto::ProtoObject*> items;
    collectElements(ctx, a, items);
    for (const proto::ProtoObject* e : items) {
        bool found = false;
        setGet(ctx, b, e, &found);
        if (!found) return false;
    }
    return true;
}

const proto::ProtoObject* prim_subset_p(proto::ProtoContext* ctx,
                                        const proto::ProtoObject*,
                                        const proto::ParentLink*,
                                        const proto::ProtoArguments* args,
                                        const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 2) throwError(ctx, "subset?: expects 2 sets");
    return isSubset(ctx, setArg(ctx, args, 0, "subset?"), setArg(ctx, args, 1, "subset?"))
        ? PROTO_TRUE : PROTO_FALSE;
}

const proto::ProtoObject* prim_superset_p(proto::ProtoContext* ctx,
                                          const proto::ProtoObject*,
                                          const proto::ParentLink*,
                                          const proto::ProtoArguments* args,
                                          const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 2) throwError(ctx, "superset?: expects 2 sets");
    return isSubset(ctx, setArg(ctx, args, 1, "superset?"), setArg(ctx, args, 0, "superset?"))
        ? PROTO_TRUE : PROTO_FALSE;
}

// Sequence helpers macros reach for --------------------------------------

// (second coll) — (first (next coll)).
const proto::ProtoObject* prim_second(proto::ProtoContext* ctx,
                                      const proto::ProtoObject*,
                                      const proto::ParentLink*,
                                      const proto::ProtoArguments* args,
                                      const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1) arityError(ctx, args, "second");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    if (isSeqObject(ctx, v)) {
        proto::ProtoContext scope(ctx->space, ctx);
        scope.resizeAutomaticLocals(1);
        SeqWalk w(&scope, 0, &scope, v);
        if (!w.done()) w.advance(&scope);
        return w.done() ? PROTO_NONE : w.first(&scope);
    }
    const proto::ProtoList* lst = asSeqOrNull(ctx, v);
    if (!lst || lst->getSize(ctx) < 2) return PROTO_NONE;
    return lst->getAt(ctx, 1);
}

// (next coll) — the elements after the first, or nil when there are none
// (rest answers an empty list there).
const proto::ProtoObject* prim_next(proto::ProtoContext* ctx,
                                    const proto::ProtoObject*,
                                    const proto::ParentLink*,
                                    const proto::ProtoArguments* args,
                                    const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1) arityError(ctx, args, "next");
    const proto::ProtoObject* r;
    if (tryNext(ctx, args->getAt(ctx, 0), &r)) return r;
    const proto::ProtoObject* s = seqOf(ctx, args->getAt(ctx, 0));
    if (s == PROTO_NONE) return PROTO_NONE;
    return seqNext(ctx, s);
}

// (last coll) — the last element, or nil.
const proto::ProtoObject* prim_last(proto::ProtoContext* ctx,
                                    const proto::ProtoObject*,
                                    const proto::ParentLink*,
                                    const proto::ProtoArguments* args,
                                    const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1) arityError(ctx, args, "last");
    const proto::ProtoObject* v = args->getAt(ctx, 0);
    if (isSeqObject(ctx, v)) {
        // Walked to the end, keeping only the current element.
        proto::ProtoContext scope(ctx->space, ctx);
        scope.resizeAutomaticLocals(2);
        SeqWalk w(&scope, 0, &scope, v);
        releaseArg(args, 0);
        scope.setAutomaticLocal(1, PROTO_NONE);
        for (; !w.done(); w.advance(&scope)) scope.setAutomaticLocal(1, w.first(&scope));
        const proto::ProtoObject* r = scope.getAutomaticLocal(1);
        scope.returnValue = r;
        return r;
    }
    const proto::ProtoList* lst = asSeqOrNull(ctx, v);
    if (!lst || lst->getSize(ctx) == 0) return PROTO_NONE;
    return lst->getLast(ctx);
}

// (seq? x) — true for a list, a lazy seq, a cons and a chunk (LazySeq.h):
// what Clojure's ISeq covers. A vector, a map or a set is not a seq.
const proto::ProtoObject* prim_seq_p(proto::ProtoContext* ctx,
                                     const proto::ProtoObject*,
                                     const proto::ParentLink*,
                                     const proto::ProtoArguments* args,
                                     const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) != 1) arityError(ctx, args, "seq?");
    return isSeqValue(ctx, args->getAt(ctx, 0)) ? PROTO_TRUE : PROTO_FALSE;
}

// (conj coll x ...) — adds each x where the collection grows: at the front
// of a list (nil is the empty list), at the end of a vector, and, for a map,
// the entry of a [k v] vector or every entry of a map. (conj) is [] and
// (conj coll) is coll.
const proto::ProtoObject* prim_conj(proto::ProtoContext* ctx,
                                    const proto::ProtoObject*,
                                    const proto::ParentLink*,
                                    const proto::ProtoArguments* args,
                                    const proto::ProtoKeywordArguments*) {
    const proto::proto_ulong n = args ? args->getSize(ctx) : 0;
    if (n == 0) return newVector(ctx, ctx->newList());
    const proto::ProtoObject* coll = args->getAt(ctx, 0);
    if (n == 1) return coll;
    if (isMap(coll)) {
        std::vector<const proto::ProtoObject*> kv;
        for (proto::proto_ulong i = 1; i < n; ++i) {
            const proto::ProtoObject* x = args->getAt(ctx, static_cast<int>(i));
            if (isMap(x)) {
                mapForEach(ctx, x, &kv,
                    [](proto::ProtoContext*, void* self, const proto::ProtoObject* k,
                       const proto::ProtoObject* v) {
                        auto* acc = static_cast<std::vector<const proto::ProtoObject*>*>(self);
                        acc->push_back(k);
                        acc->push_back(v);
                    });
            } else if (isVector(x) && vectorItems(ctx, x)->getSize(ctx) == 2) {
                kv.push_back(vectorItems(ctx, x)->getAt(ctx, 0));
                kv.push_back(vectorItems(ctx, x)->getAt(ctx, 1));
            } else if (x && x != PROTO_NONE) {
                throwError(ctx, "IllegalArgumentException: Vector arg to map conj must be a pair");
            }
        }
        // Every key and value is reachable from the rooted arguments.
        return mapAssocPairs(ctx, coll, kv.data(), kv.size());
    }
    if (isSet(ctx, coll)) {
        // Every element is reachable from the rooted arguments.
        return setConj(ctx, coll, args->values() + 1, n - 1);
    }
    if (isVector(coll)) {
        proto::ProtoContext::CriticalSection guard(ctx);
        const proto::ProtoList* items = vectorItems(ctx, coll);
        for (proto::proto_ulong i = 1; i < n; ++i)
            items = items->appendLast(ctx, args->getAt(ctx, static_cast<int>(i)));
        return newVector(ctx, items);
    }
    if (isSeqObject(ctx, coll)) {
        // A seq grows at the front, one cons per element, as in Clojure.
        proto::ProtoContext scope(ctx->space, ctx);
        scope.resizeAutomaticLocals(1);
        scope.setAutomaticLocal(0, coll);
        for (proto::proto_ulong i = 1; i < n; ++i)
            scope.setAutomaticLocal(0, newCons(&scope, args->getAt(&scope, static_cast<int>(i)),
                                               scope.getAutomaticLocal(0)));
        const proto::ProtoObject* r = scope.getAutomaticLocal(0);
        scope.returnValue = r;
        return r;
    }
    if (coll && coll != PROTO_NONE && !isListTag(coll))
        throwError(ctx, "ClassCastException: conj expects a collection, got " +
                        std::string(valueTypeName(ctx, coll)));
    proto::ProtoContext::CriticalSection guard(ctx);
    const proto::ProtoList* lst = (coll && coll != PROTO_NONE) ? coll->asList(ctx) : ctx->newList();
    for (proto::proto_ulong i = 1; i < n; ++i)
        lst = lst->appendFirst(ctx, args->getAt(ctx, static_cast<int>(i)));
    return lst->asObject(ctx);
}

// (pr-str x ...) — the arguments printed readably, separated by spaces, as
// the REPL prints a result: `(pr-str "a" :b)` is "\"a\" :b".
const proto::ProtoObject* prim_pr_str(proto::ProtoContext* ctx,
                                      const proto::ProtoObject*,
                                      const proto::ParentLink*,
                                      const proto::ProtoArguments* args,
                                      const proto::ProtoKeywordArguments*) {
    const proto::proto_ulong n = args ? args->getSize(ctx) : 0;
    std::string text;
    for (proto::proto_ulong i = 0; i < n; ++i) {
        if (i > 0) text += ' ';
        printTo(ctx, text, args->getAt(ctx, static_cast<int>(i)), /*readable=*/true);
    }
    return makeUtf8String(ctx, text);
}

// (pr x ...) — the arguments printed readably, as prn does, without the
// newline.
const proto::ProtoObject* prim_pr(proto::ProtoContext* ctx,
                                  const proto::ProtoObject*,
                                  const proto::ParentLink*,
                                  const proto::ProtoArguments* args,
                                  const proto::ProtoKeywordArguments*) {
    const proto::proto_ulong n = args ? args->getSize(ctx) : 0;
    std::string text;
    for (proto::proto_ulong i = 0; i < n; ++i) {
        if (i > 0) text += ' ';
        printTo(ctx, text, args->getAt(ctx, static_cast<int>(i)), /*readable=*/true);
    }
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
    return PROTO_NONE;
}

// format / printf ---------------------------------------------------------
//
// Java's java.util.Formatter, which Clojure's format calls, for the
// conversions a script uses: %s %S %d %x %X %o %f %e %E %g %G %b %B %c %n %%,
// with an argument index (`%2$s`, `%<s`), the flags `-` `0` `+` ` ` `,` `#`
// `(`, a width and a precision. A conversion that does not fit its argument
// raises IllegalArgumentException with Java's exception name
// (`IllegalFormatConversionException: d != a float`), as JVM Clojure does.

[[noreturn]] void throwFormat(proto::ProtoContext* ctx, const std::string& what) {
    throwClassed(ctx, "IllegalArgumentException", what);
}

// Inserts Java's `,` grouping into the integer digits of `digits` (which may
// start with a sign and contain a decimal point).
std::string groupThousands(const std::string& digits) {
    std::size_t start = (!digits.empty() && (digits[0] == '-' || digits[0] == '+')) ? 1 : 0;
    std::size_t end = digits.find('.');
    if (end == std::string::npos) end = digits.size();
    std::string intPart = digits.substr(start, end - start);
    std::string grouped;
    const std::size_t n = intPart.size();
    for (std::size_t i = 0; i < n; ++i) {
        grouped += intPart[i];
        if (i + 1 < n && (n - i - 1) % 3 == 0) grouped += ',';
    }
    return digits.substr(0, start) + grouped + digits.substr(end);
}

// Pads `body` to `width`: left-justified with `-`, with zeros after the sign
// with `0`, else with spaces on the left.
std::string padField(std::string body, long width, bool left, bool zero) {
    if (width < 0 || static_cast<long>(body.size()) >= width) return body;
    const std::size_t fill = static_cast<std::size_t>(width) - body.size();
    if (left) return body + std::string(fill, ' ');
    if (zero) {
        std::size_t at = (!body.empty() && (body[0] == '-' || body[0] == '+' || body[0] == ' ' || body[0] == '(')) ? 1 : 0;
        if (body.compare(at, 2, "0x") == 0 || body.compare(at, 2, "0X") == 0) at += 2;
        return body.substr(0, at) + std::string(fill, '0') + body.substr(at);
    }
    return std::string(fill, ' ') + body;
}

// `m` (finite, non-negative) with `prec` fractional digits ('f') or in
// scientific notation with `prec` mantissa digits ('e'), rounded half up as
// Java's Formatter rounds (RoundingMode.HALF_UP), where C's printf rounds an
// exact tie to even: (format "%.1f" 2.25) is "2.3" in Java and "2.2" in C.
// glibc prints exact decimal digits, so 25 extra digits decide the tie.
std::string fixedHalfUp(double m, int prec, char conv) {
    const int extra = 25;
    const char* cfmt = conv == 'f' ? "%.*f" : "%.*e";
    std::string text(static_cast<std::size_t>(std::snprintf(nullptr, 0, cfmt, prec + extra, m)) + 1, '\0');
    std::snprintf(text.data(), text.size(), cfmt, prec + extra, m);
    text.pop_back();
    std::string exponent;
    if (conv == 'e') {
        const std::size_t e = text.find('e');
        exponent = text.substr(e);
        text = text.substr(0, e);
    }
    const std::size_t point = text.find('.');
    std::string digits = text.substr(0, point) + text.substr(point + 1);
    const std::size_t intLen = point;
    const std::size_t keep = intLen + static_cast<std::size_t>(prec);
    const bool up = digits[keep] >= '5';
    digits.resize(keep);
    std::size_t newIntLen = intLen;
    if (up) {
        std::size_t i = keep;
        while (i > 0) {
            --i;
            if (digits[i] == '9') { digits[i] = '0'; continue; }
            ++digits[i];
            break;
        }
        if (digits.empty() || (i == 0 && digits[0] == '0')) {
            digits.insert(digits.begin(), '1');
            ++newIntLen;
        }
    }
    if (conv == 'e' && newIntLen > intLen) {
        // 9.99e+00 rounded up is 10.0e+00: renormalise to 1.00e+01.
        digits.pop_back();
        newIntLen = intLen;
        int ex = std::stoi(exponent.substr(1)) + 1;
        char eb[16];
        std::snprintf(eb, sizeof eb, "e%c%02d", ex < 0 ? '-' : '+', ex < 0 ? -ex : ex);
        exponent = eb;
    }
    std::string out = digits.substr(0, newIntLen);
    if (prec > 0) out += "." + digits.substr(newIntLen);
    return out + exponent;
}

std::string formatValue(proto::ProtoContext* ctx, const std::string& flags, long width,
                        long precision, char conv, const proto::ProtoObject* arg) {
    const bool left = flags.find('-') != std::string::npos;
    const bool zero = flags.find('0') != std::string::npos;
    const bool plus = flags.find('+') != std::string::npos;
    const bool space = flags.find(' ') != std::string::npos;
    const bool group = flags.find(',') != std::string::npos;
    const bool alt = flags.find('#') != std::string::npos;
    const bool paren = flags.find('(') != std::string::npos;
    const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(conv)));
    const bool upper = conv != lower;
    auto upcase = [](std::string t) {
        for (char& c : t) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return t;
    };
    auto typeName = [&]() -> std::string {
        return valueTypeName(ctx, arg);
    };
    std::string body;
    switch (lower) {
        case 's': {
            if (!arg || arg == PROTO_NONE) body = "null";
            else appendStr(ctx, body, arg);
            if (precision >= 0 && static_cast<long>(body.size()) > precision)
                body.resize(static_cast<std::size_t>(precision));
            if (upper) body = upcase(body);
            return padField(body, width, left, false);
        }
        case 'b': {
            body = (!arg || arg == PROTO_NONE || arg == PROTO_FALSE) ? "false" : "true";
            if (upper) body = upcase(body);
            return padField(body, width, left, false);
        }
        case 'c': {
            if (isStringLike(arg)) body = asProtoString(arg)->toStdString(ctx);
            else if (arg && arg->isInteger(ctx)) {
                const long long cp = arg->asLong(ctx);
                // The code point, UTF-8 encoded.
                if (cp < 0x80) body += static_cast<char>(cp);
                else if (cp < 0x800) { body += static_cast<char>(0xC0 | (cp >> 6)); body += static_cast<char>(0x80 | (cp & 0x3F)); }
                else if (cp < 0x10000) { body += static_cast<char>(0xE0 | (cp >> 12)); body += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); body += static_cast<char>(0x80 | (cp & 0x3F)); }
                else { body += static_cast<char>(0xF0 | (cp >> 18)); body += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); body += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); body += static_cast<char>(0x80 | (cp & 0x3F)); }
            } else if (!arg || arg == PROTO_NONE) body = "null";
            else throwFormat(ctx, std::string("IllegalFormatConversionException: c != ") + typeName());
            if (upper) body = upcase(body);
            return padField(body, width, left, false);
        }
        case 'd': case 'x': case 'o': {
            if (!arg || arg == PROTO_NONE) return padField("null", width, left, false);
            if (!arg->isInteger(ctx))
                throwFormat(ctx, std::string("IllegalFormatConversionException: ") + conv + " != " + typeName());
            bool negative = false;
            std::string digits;
            const int base = lower == 'd' ? 10 : lower == 'x' ? 16 : 8;
            bool small = true;
            long long v = 0;
            try {
                DebugQuietScope probe;   // a probe, not the program's exception
                v = arg->asLong(ctx);
            } catch (const proto::ProtoException&) { small = false; }
            if (small && base != 10) {
                // A long prints in two's complement, as Java's %x of a Long.
                char buf[32];
                std::snprintf(buf, sizeof buf, base == 16 ? "%llx" : "%llo",
                              static_cast<unsigned long long>(v));
                digits = buf;
            } else if (small) {
                negative = v < 0;
                digits = std::to_string(v);
                if (negative) digits.erase(0, 1);
            } else {
                digits = arg->asIntegerString(ctx, base)->toStdString(ctx);
                if (!digits.empty() && digits[0] == '-') { negative = true; digits.erase(0, 1); }
            }
            if (lower == 'd' && group) digits = groupThousands(digits);
            if (alt && lower == 'x') digits = "0x" + digits;
            if (alt && lower == 'o') digits = "0" + digits;
            if (negative) body = paren ? "(" + digits + ")" : "-" + digits;
            else body = (plus ? "+" : space ? " " : "") + digits;
            if (upper) body = upcase(body);
            return padField(body, width, left, zero);
        }
        case 'f': case 'e': case 'g': {
            if (!arg || arg == PROTO_NONE) return padField("null", width, left, false);
            if (!arg->isFloat(ctx))
                throwFormat(ctx, std::string("IllegalFormatConversionException: ") + conv + " != " + typeName());
            const double d = arg->asDouble(ctx);
            if (std::isnan(d) || std::isinf(d)) {
                body = std::isnan(d) ? "NaN" : (d > 0 ? (plus ? "+Infinity" : "Infinity") : (paren ? "(Infinity)" : "-Infinity"));
                return padField(body, width, left, false);
            }
            const int prec = precision >= 0 ? static_cast<int>(precision) : 6;
            std::string digits;
            if (lower == 'g') {
                char buf[512];
                std::snprintf(buf, sizeof buf, "%#.*g", prec == 0 ? 1 : prec, std::fabs(d));
                digits = buf;
            } else {
                digits = fixedHalfUp(std::fabs(d), prec, lower);
            }
            if (lower == 'f' && group) digits = groupThousands(digits);
            const bool negative = std::signbit(d);
            if (negative) body = paren ? "(" + digits + ")" : "-" + digits;
            else body = (plus ? "+" : space ? " " : "") + digits;
            if (upper) body = upcase(body);
            return padField(body, width, left, zero);
        }
        default:
            throwFormat(ctx, std::string("UnknownFormatConversionException: Conversion = '") + conv + "'");
    }
}

std::string formatString(proto::ProtoContext* ctx, const std::string& fmt,
                         const proto::ProtoArguments* args, proto::proto_ulong first) {
    const proto::proto_ulong argc = args ? args->getSize(ctx) : 0;
    std::string out;
    proto::proto_ulong next = first;   // the next ordinary argument
    long last = -1;                    // the argument `%<` reuses
    for (std::size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != '%') { out += fmt[i]; continue; }
        std::size_t j = i + 1;
        // %[index$][flags][width][.precision]conversion
        long index = -1;
        bool previous = false;
        std::size_t k = j;
        while (k < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[k]))) ++k;
        if (k < fmt.size() && fmt[k] == '$' && k > j) {
            index = std::stol(fmt.substr(j, k - j));
            j = k + 1;
        } else if (j < fmt.size() && fmt[j] == '<') {
            previous = true;
            ++j;
        }
        std::string flags;
        while (j < fmt.size() && std::string("-#+ 0,(").find(fmt[j]) != std::string::npos) flags += fmt[j++];
        long width = -1, precision = -1;
        if (j < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[j]))) {
            std::size_t w = j;
            while (j < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[j]))) ++j;
            width = std::stol(fmt.substr(w, j - w));
        }
        if (j < fmt.size() && fmt[j] == '.') {
            std::size_t p = ++j;
            while (j < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[j]))) ++j;
            if (j == p) throwFormat(ctx, "IllegalFormatPrecisionException: missing precision");
            precision = std::stol(fmt.substr(p, j - p));
        }
        if (j >= fmt.size()) throwFormat(ctx, "UnknownFormatConversionException: Conversion = '%'");
        const char conv = fmt[j];
        i = j;
        if (conv == '%') { out += padField("%", width, flags.find('-') != std::string::npos, false); continue; }
        if (conv == 'n') { out += '\n'; continue; }
        proto::proto_ulong at;
        if (previous) {
            if (last < 0) throwFormat(ctx, "MissingFormatArgumentException: Format specifier '%<" + std::string(1, conv) + "'");
            at = static_cast<proto::proto_ulong>(last);
        } else if (index > 0) {
            at = first + static_cast<proto::proto_ulong>(index - 1);
        } else {
            at = next++;
        }
        if (at >= argc)
            throwFormat(ctx, std::string("MissingFormatArgumentException: Format specifier '%") + conv + "'");
        last = static_cast<long>(at);
        out += formatValue(ctx, flags, width, precision, conv, args->getAt(ctx, static_cast<int>(at)));
    }
    return out;
}

// (format fmt & args) — the string Java's String.format builds.
const proto::ProtoObject* prim_format(proto::ProtoContext* ctx,
                                      const proto::ProtoObject*,
                                      const proto::ParentLink*,
                                      const proto::ProtoArguments* args,
                                      const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) < 1) throwError(ctx, "format: expects (format fmt & args)");
    const std::string fmt = stringArgOf(ctx, args, 0, "format");
    return reinterpret_cast<const proto::ProtoObject*>(
        makeUtf8ProtoString(ctx, formatString(ctx, fmt, args, 1)));
}

// (printf fmt & args) — prints (format fmt args...), with no newline.
const proto::ProtoObject* prim_printf(proto::ProtoContext* ctx,
                                      const proto::ProtoObject*,
                                      const proto::ParentLink*,
                                      const proto::ProtoArguments* args,
                                      const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) < 1) throwError(ctx, "printf: expects (printf fmt & args)");
    const std::string fmt = stringArgOf(ctx, args, 0, "printf");
    const std::string text = formatString(ctx, fmt, args, 1);
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
    return PROTO_NONE;
}

// (print x ...) — the arguments as println prints them, without the newline.
const proto::ProtoObject* prim_print(proto::ProtoContext* ctx,
                                     const proto::ProtoObject*,
                                     const proto::ParentLink*,
                                     const proto::ProtoArguments* args,
                                     const proto::ProtoKeywordArguments*) {
    const proto::proto_ulong n = args ? args->getSize(ctx) : 0;
    std::string text;
    for (proto::proto_ulong i = 0; i < n; ++i) {
        if (i > 0) text += ' ';
        printTo(ctx, text, args->getAt(ctx, static_cast<int>(i)), /*readable=*/false);
    }
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
    return PROTO_NONE;
}

// (prn x ...) — pr-str of the arguments, then a newline, to standard output.
const proto::ProtoObject* prim_prn(proto::ProtoContext* ctx,
                                   const proto::ProtoObject*,
                                   const proto::ParentLink*,
                                   const proto::ProtoArguments* args,
                                   const proto::ProtoKeywordArguments*) {
    const proto::proto_ulong n = args ? args->getSize(ctx) : 0;
    std::string line;
    for (proto::proto_ulong i = 0; i < n; ++i) {
        if (i > 0) line += ' ';
        printTo(ctx, line, args->getAt(ctx, static_cast<int>(i)), /*readable=*/true);
    }
    line += '\n';
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
    return PROTO_NONE;
}

// The VM's direct primitive calls (Primitives.h, fastPrimitiveCall): the
// values each of these primitives reads without running Clojure code.
namespace {
const proto::ProtoObject* fastIndexed(proto::ProtoContext* ctx, const proto::ProtoObject* coll,
                                      const proto::ProtoObject* i) {
    if (!proto::isSmallInt(i)) return nullptr;
    const bool vector = isVector(coll);
    if (!vector && !isListTag(coll)) return nullptr;
    const proto::ProtoList* lst = vector ? vectorItems(ctx, coll) : coll->asList(ctx);
    const long long idx = proto::asSmallInt(i);
    if (idx < 0 || idx >= static_cast<long long>(lst->getSize(ctx))) return nullptr;
    return lst->getAt(ctx, static_cast<int>(idx));
}
} // namespace

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
    {"dissoc",    &prim_dissoc},
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
    {"seq",     &prim_seq},
    {"concat",  &prim_concat},
    {"symbol?", &prim_symbol_p},
    {"keyword?", &prim_keyword_p},
    {"reduce",  &prim_reduce},

    // Exceptions (Exceptions.h).
    {"ex-info",    &prim_ex_info},
    {"ex-data",    &prim_ex_data},
    {"ex-message", &prim_ex_message},
    {"ex-cause",   &prim_ex_cause},

    // Modules (Unified Module Discovery).
    {"import-module", &prim_import_module},

    // Macros and eval (Macros.h, compiler/Eval.h).
    {"__macro__",     &prim_make_macro},
    {"macroexpand-1", &prim_macroexpand_1},
    {"macroexpand",   &prim_macroexpand},
    {"eval",          &prim_eval},

    // Sequences and printing.
    {"second",  &prim_second},
    {"next",    &prim_next},
    {"last",    &prim_last},
    {"seq?",    &prim_seq_p},
    {"conj",    &prim_conj},
    {"pr-str",  &prim_pr_str},
    {"prn",     &prim_prn},
    {"print",   &prim_print},
    {"pr",      &prim_pr},

    // Symbols (syntax-quote's companions).
    {"gensym",  &prim_gensym},
    {"symbol",  &prim_symbol},
    {"keyword", &prim_keyword},
    {"name",    &prim_name},
    {"list*",   &prim_list_star},

    // Numbers, and the helper the core.clj macros raise errors with.
    {"even?",   &prim_even_p},
    {"odd?",    &prim_odd_p},
    {"__raise-illegal-argument", &prim_raise_illegal_argument},

    // Sets (SetOps.h) and clojure.set, in the global namespace (D28).
    {"hash-set",     &prim_hash_set},
    {"set",          &prim_set},
    {"set?",         &prim_set_p},
    {"disj",         &prim_disj},
    {"union",        &prim_union},
    {"intersection", &prim_intersection},
    {"difference",   &prim_difference},
    {"subset?",      &prim_subset_p},
    {"superset?",    &prim_superset_p},

    // Regular expressions (Regex.h).
    {"re-pattern",    &prim_re_pattern},
    {"re-matcher",    &prim_re_matcher},
    {"re-find",       &prim_re_find},
    {"re-matches",    &prim_re_matches},
    {"re-seq",        &prim_re_seq},
    {"re-groups",     &prim_re_groups},
    {"replace-first", &prim_replace_first},
    {"identical?",    &prim_identical_p},

    // format / printf.
    {"format",        &prim_format},
    {"printf",        &prim_printf},

    // Destructuring (compiler/Compiler.h, destructure).
    {"__destructure-map", &prim_destructure_map},
};

// A linear scan of the entries; printing a function is not a hot path.
const char* primitiveName(proto::ProtoMethod fn) {
    for (const PrimitiveEntry& p : kPrimitives) {
        if (p.fn == fn) return p.name;
    }
    if (const char* lazy = lazySeqPrimitiveName(fn)) return lazy;
    if (const char* core = corePrimitiveName(fn)) return core;
    if (const char* data = dataFormatPrimitiveName(fn)) return data;
    return ioPrimitiveName(reinterpret_cast<const void*>(fn));
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

// Declared in Primitives.h.
const proto::ProtoList* seqItems(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
    return asSeqOrNull(ctx, v);
}

// Declared in LazySeq.h: the list seqOf answers for a map, a set or a string.
bool collectionItems(proto::ProtoContext* ctx, const proto::ProtoObject* v,
                     const proto::ProtoList** out) {
    if (!isMap(v) && !isSet(ctx, v) && !isStringLike(v)) return false;
    ListBuilder b(ctx);
    pushSeqElements(b, v);
    *out = b.size() == 0 ? nullptr : b.finishList();
    return true;
}

// Declared in Primitives.h.
const proto::ProtoObject* numberAdd(proto::ProtoContext* ctx, const proto::ProtoObject* a,
                                    const proto::ProtoObject* b) {
    if (!isNumber(ctx, a)) throwNotANumber(ctx, "+", a);
    if (!isNumber(ctx, b)) throwNotANumber(ctx, "+", b);
    return a->add(ctx, b);
}

int numberCompare(proto::ProtoContext* ctx, const proto::ProtoObject* a,
                  const proto::ProtoObject* b) {
    if (!isNumber(ctx, a)) throwNotANumber(ctx, "<", a);
    if (!isNumber(ctx, b)) throwNotANumber(ctx, "<", b);
    if (a->isInteger(ctx) && b->isInteger(ctx)) return compareIntegers(ctx, a, b);
    const double x = a->asDouble(ctx), y = b->asDouble(ctx);
    return x < y ? -1 : (x > y ? 1 : 0);
}

// Declared in Primitives.h; the exception printer's way back into printTo.
void printValueTo(proto::ProtoContext* ctx, std::string& out,
                  const proto::ProtoObject* v, bool readable) {
    printTo(ctx, out, v, readable);
}

// Declared in Primitives.h; used by `deliver` and the actor scheduler.
bool deliverPromise(proto::ProtoContext* ctx, const proto::ProtoString* valueKey,
                    const proto::ProtoObject* promise,
                    const proto::ProtoObject* value) {
    // The one-element list is rooted in a child context slot across the
    // compare-and-set, which allocates.
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(1);
    scope.setAutomaticLocal(0,
        scope.newList()->appendLast(&scope, value ? value : PROTO_NONE)
            ->asObject(&scope));
    return promise->setAttributeIfEqual(&scope, valueKey, nullptr,
                                        scope.getAutomaticLocal(0));
}

namespace {

// A list or a vector read through one indexed interface, so the sequential
// rules below treat both concrete types alike. Every sequence the runtime
// produces (`rest`, `map`, `filter`, `keys`, `cons`, ...) is one of the two.
struct SequentialView {
    const proto::ProtoList* list = nullptr;

    explicit operator bool() const { return list != nullptr; }
    proto::proto_ulong size(proto::ProtoContext* ctx) const { return list->getSize(ctx); }
    // O(log n): protoCore exposes no allocation-free sequential walk of a
    // ProtoList, so elements are read by index.
    const proto::ProtoObject* at(proto::ProtoContext* ctx, proto::proto_ulong i) const {
        return list->getAt(ctx, static_cast<int>(i));
    }
};

SequentialView sequentialView(proto::ProtoContext* ctx,
                              const proto::ProtoObject* v) {
    SequentialView s;
    if (isListTag(v))      s.list = v->asList(ctx);
    else if (isVector(v))  s.list = vectorItems(ctx, v);
    return s;
}

} // namespace

const char* valueTypeName(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
    if (!v || v == PROTO_NONE)              return "nil";
    if (v == PROTO_TRUE || v == PROTO_FALSE) return "a boolean";
    if (v->isInteger(ctx))                  return "an integer";
    if (v->isDouble(ctx))                   return "a float";
    if (proto::ProtoObject::isStringTagFast(v)) return "a string";
    if (isListTag(v))                       return "a list";
    if (isVector(v))                        return "a vector";
    switch (seqObjectKind(ctx, v)) {
        case SeqObject::Lazy:  return "a lazy seq";
        case SeqObject::Cons:
        case SeqObject::Chunk: return "a seq";
        case SeqObject::None:  break;
    }
    if (isMap(v))                           return "a map";
    if (isSet(ctx, v))                      return "a set";
    if (isPattern(ctx, v))                  return "a pattern";
    if (isMatcher(ctx, v))                  return "a matcher";
    if (v->isMethod(ctx))                   return "a fn";
    if (isException(ctx, v))                return "an exception";
    if (const char* io = ioHandleTypeName(ctx, v)) return io;
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) return "an object";
    if (isNamed(ctx, cc->named, v)) {
        // A keyword's spelling starts with ':', a symbol's never does.
        const std::string spelling =
            namedSpelling(ctx, cc->named, v)->toStdString(ctx);
        return (!spelling.empty() && spelling[0] == ':') ? "a keyword" : "a symbol";
    }
    const proto::ProtoObject* prototype = v->getPrototype(ctx);
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
    throwClassed(ctx, "ClassCastException", std::string(operation) +
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
bool valuesEqual(proto::ProtoContext* ctx,
                 const proto::ProtoObject* a, const proto::ProtoObject* b) {
    if (!a) a = PROTO_NONE;
    if (!b) b = PROTO_NONE;
    if (a == b) return true;
    // Recurses once per level of nesting (StackGuard.h).
    checkNativeStack();

    // Maps: the same keys, with `=` values (MapOps.h).
    const bool aMap = isMap(a);
    const bool bMap = isMap(b);
    if (aMap || bMap) {
        return aMap && bMap &&
            mapEquals(ctx, a, b, nullptr,
                [](proto::ProtoContext* c, void*,
                   const proto::ProtoObject* x, const proto::ProtoObject* y) {
                    return valuesEqual(c, x, y);
                });
    }

    // Sets: the same elements, matched as map keys are (SetOps.h), so
    // (= #{1} #{1.0}) is false, as in Clojure, whose sets hash 1 and 1.0
    // differently.
    const bool aSet = isSet(ctx, a);
    const bool bSet = isSet(ctx, b);
    if (aSet || bSet) return aSet && bSet && setEquals(ctx, a, b);

    // Any other object — keywords and symbols (interned, Named.h), atoms,
    // functions, ... — is equal only to itself. Decided by the pointer tag:
    // protoCore's isString and compare would first probe the
    // object for a `__data__` wrapper attribute, which the runtime never sets.
    if ((isObjectTag(a) && !isSeqObject(ctx, a)) || (isObjectTag(b) && !isSeqObject(ctx, b)))
        return false;

    // Sequential collections (lists and vectors) are equal when they hold
    // equal elements in the same order, whatever their concrete types, and
    // are never equal to anything else. One pass over the elements that
    // stops at the first mismatch. Since vectors stopped being interned
    // tuples, two equal vectors are no longer one pointer, so this walk is
    // what decides them.
    // A lazy seq, a cons or a chunk is sequential too: two walks in step,
    // stopping at the first mismatch (only the elements compared are
    // realised).
    if (isSeqObject(ctx, a) || isSeqObject(ctx, b)) {
        if (!(isSeqValue(ctx, a) || isVector(a)) || !(isSeqValue(ctx, b) || isVector(b)))
            return false;
        proto::ProtoContext scope(ctx->space, ctx);
        scope.resizeAutomaticLocals(2);
        SeqWalk wa(&scope, 0, &scope, a);
        SeqWalk wb(&scope, 1, &scope, b);
        for (; !wa.done() && !wb.done(); wa.advance(&scope), wb.advance(&scope)) {
            if (!valuesEqual(&scope, wa.first(&scope), wb.first(&scope))) return false;
        }
        return wa.done() && wb.done();
    }
    const SequentialView sa = sequentialView(ctx, a);
    const SequentialView sb = sequentialView(ctx, b);
    if (sa || sb) {
        if (!sa || !sb) return false;
        const proto::proto_ulong n = sa.size(ctx);
        if (n != sb.size(ctx)) return false;
        for (proto::proto_ulong i = 0; i < n; ++i) {
            if (!valuesEqual(ctx, sa.at(ctx, i), sb.at(ctx, i)))
                return false;
        }
        return true;
    }

    // Numbers by value across types (D15) with IEEE semantics, so NaN is equal
    // to no number, itself included (the identity test above still makes one
    // NaN object `=` to itself, as JVM Clojure's Util.equiv does); strings by
    // content; any other pair only when identical.
    return a->partialCompare(ctx, b) == 0;
}

void installPrimitives(proto::ProtoContext* ctx,
                       proto::ProtoObject* globals) {
    // The exception marker and the in-flight root set (Exceptions.h).
    initExceptions(ctx, globals);
    // The pattern and matcher prototypes (Regex.h), likewise.
    globals->setAttribute(ctx, proto::ProtoString::createSymbol(ctx, "__regex_prototypes__"),
                          initRegex(ctx));
    // The set prototype (SetOps.h), reachable from the globals, which every
    // entry point roots.
    globals->setAttribute(ctx, proto::ProtoString::createSymbol(ctx, "__set_prototype__"),
                          initSetOps(ctx));
    // The lazy seq, cons and chunk prototypes (SeqTypes.h), likewise, and
    // the lazy-sequence functions (LazySeq.h).
    globals->setAttribute(ctx, proto::ProtoString::createSymbol(ctx, "__seq_prototypes__"),
                          initSeqTypes(ctx));
    initLazySeqs(ctx);
    installLazySeqPrimitives(ctx, globals);
    registerStep(StepKind::ReSeq, &stepReSeq, /*pure=*/true);
    // Install each primitive: wrap the C function pointer in a callable
    // ProtoObject via fromMethod, store on the globals under the symbol key.
    // setAttribute on a mutable receiver mutates in place.
    for (const PrimitiveEntry& p : kPrimitives) {
        const proto::ProtoObject* callable =
            ctx->fromMethod(nullptr /* self */, p.fn);
        defineBuiltin(ctx, globals, p.name, callable);
    }
    // Input and output (IO.h).
    installIO(ctx, globals);
    // The Part C vocabulary in C++ (CorePrims.h), after the I/O globals,
    // which its `System` object reads.
    installCorePrimitives(ctx, globals);
    // read-string, clojure.edn, cheshire.core, clojure.data.json
    // (DataFormats.h).
    installEdn(ctx, globals);
    installJson(ctx, globals);
}

} // namespace protoClojure

namespace protoClojure {

const proto::ProtoObject* fastPrimitiveCall(proto::ProtoContext* ctx, proto::ProtoMethod fn,
                                            const proto::ProtoObject* const* argv, unsigned argc,
                                            bool mayAllocate) {
    const proto::ProtoObject* r = nullptr;
    if (argc == 1) {
        const proto::ProtoObject* v = argv[0];
        if (fn == &prim_inc || fn == &prim_dec) {
            if (!proto::isSmallInt(v)) return nullptr;
            const long long n = proto::asSmallInt(v) + (fn == &prim_inc ? 1 : -1);
            return proto::smallIntInRange(n) ? proto::makeSmallInt(n) : nullptr;
        }
        if (fn == &prim_first) return tryFirst(ctx, v, &r) ? r : nullptr;
        if (!mayAllocate) {
            if (fn == &prim_rest || fn == &prim_next || fn == &prim_seq) return nullptr;
        }
        if (fn == &prim_rest)  return tryRest(ctx, v, &r) ? r : nullptr;
        if (fn == &prim_next)  return tryNext(ctx, v, &r) ? r : nullptr;
        if (fn == &prim_seq)   return trySeq(ctx, v, &r) ? r : nullptr;
        if (fn == &prim_empty_p) {
            bool e;
            return tryEmpty(ctx, v, &e) ? (e ? PROTO_TRUE : PROTO_FALSE) : nullptr;
        }
        if (fn == &prim_count) {
            if (!v || v == PROTO_NONE) return proto::makeSmallInt(0);
            if (isListTag(v))
                return proto::makeSmallInt(static_cast<long long>(v->asList(ctx)->getSize(ctx)));
            if (isVector(v))
                return proto::makeSmallInt(static_cast<long long>(vectorItems(ctx, v)->getSize(ctx)));
            return nullptr;
        }
        return nullptr;
    }
    if (argc == 2) {
        if (fn == &prim_nth) return fastIndexed(ctx, argv[0], argv[1]);
        if (fn == &prim_plus) {
            if (!proto::isSmallInt(argv[0]) || !proto::isSmallInt(argv[1])) return nullptr;
            const long long n = proto::asSmallInt(argv[0]) + proto::asSmallInt(argv[1]);
            return proto::smallIntInRange(n) ? proto::makeSmallInt(n) : nullptr;
        }
    }
    return nullptr;
}

} // namespace protoClojure
