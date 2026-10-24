// CorePrims — the scripting vocabulary written in C++ (macros brief, Part C;
// docs/DESIGN.md §10.2, M32-M34): integer division (`quot`, `rem`, `mod`),
// `compare`, `max`, `min`, `sort`, `sort-by`, `frequencies`,
// `clojure.string/split-lines`, `require` (namespace aliases), `parse-long`,
// `parse-double`, `parse-boolean`, `bigint`, the auto-promoting `+'` family,
// and the
// `System` object (`(System/exit n)`, `System/getenv`,
// `System/currentTimeMillis`, `System/nanoTime`). The rest of Part C is
// Clojure, in core.clj.
//
// C++ where the work is per element and a Clojure definition would pay a
// frame per comparison or per assoc (sort, frequencies) or where Clojure's
// numeric semantics need protoCore's exact integers (quot, rem, mod).

#include "CorePrims.h"

#include "Exceptions.h"
#include "ExecutionEngine.h"
#include "LazySeq.h"
#include "Macros.h"
#include "MapOps.h"
#include "Named.h"
#include "Primitives.h"
#include "Utf8String.h"
#include "SetOps.h"
#include "VectorOps.h"

#include "protoCore.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <system_error>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

namespace protoClojure {

namespace {

using PO = proto::ProtoObject;

#define COREPRIM(name) \
    const PO* name(proto::ProtoContext* ctx, const PO*, const proto::ParentLink*, \
                   const proto::ProtoArguments* args, const proto::ProtoKeywordArguments*)

proto::proto_ulong argc(proto::ProtoContext* ctx, const proto::ProtoArguments* args) {
    return args ? args->getSize(ctx) : 0;
}
const PO* argAt(proto::ProtoContext* ctx, const proto::ProtoArguments* args, proto::proto_ulong i) {
    const PO* v = args->getAt(ctx, static_cast<int>(i));
    return v ? v : PROTO_NONE;
}
[[noreturn]] void arity(proto::ProtoContext* ctx, proto::proto_ulong n, const char* name) {
    throwClassed(ctx, "ArityException",
                 "Wrong number of args (" + std::to_string(n) + ") passed to: " + name);
}
bool truthy(const PO* v) { return v && v != PROTO_NONE && v != PROTO_FALSE; }

const ActiveCallContext* callContext(proto::ProtoContext* ctx, const char* who) {
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throwError(ctx, std::string(who) + ": no active VM context");
    return cc;
}

// ---- quot / rem / mod ---------------------------------------------------------

enum class DivKind { Quot, Rem, Mod };

// Clojure's quot (truncated toward zero), rem (sign of the dividend) and mod
// (sign of the divisor), exact for integers of any size, IEEE for floats;
// a zero integer divisor raises ArithmeticException "Divide by zero".
const PO* intDivision(proto::ProtoContext* ctx, const proto::ProtoArguments* args,
                      DivKind kind, const char* name) {
    if (argc(ctx, args) != 2) arity(ctx, argc(ctx, args), name);
    const PO* a = argAt(ctx, args, 0);
    const PO* b = argAt(ctx, args, 1);
    if (!isNumber(ctx, a)) throwNotANumber(ctx, name, a);
    if (!isNumber(ctx, b)) throwNotANumber(ctx, name, b);
    if (a->isFloat(ctx) || b->isFloat(ctx)) {
        const double x = a->asDouble(ctx), y = b->asDouble(ctx);
        if (y == 0.0) throwClassed(ctx, "ArithmeticException", "Divide by zero");
        const double q = std::trunc(x / y);
        if (kind == DivKind::Quot) return ctx->fromDouble(q);
        double r = std::fmod(x, y);
        if (kind == DivKind::Mod && r != 0.0 && ((r < 0) != (y < 0))) r += y;
        return ctx->fromDouble(r);
    }
    if (proto::isSmallInt(a) && proto::isSmallInt(b)) {
        const long long x = proto::asSmallInt(a), y = proto::asSmallInt(b);
        if (y == 0) throwClassed(ctx, "ArithmeticException", "Divide by zero");
        if (kind == DivKind::Quot) return ctx->fromLong(x / y);
        long long r = x % y;
        if (kind == DivKind::Mod && r != 0 && ((r < 0) != (y < 0))) r += y;
        return ctx->fromLong(r);
    }
    // Large integers: protoCore's divide truncates toward zero, as quot.
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(2);
    scope.setAutomaticLocal(0, a->divide(&scope, b));
    if (kind == DivKind::Quot) {
        scope.returnValue = scope.getAutomaticLocal(0);
        return scope.getAutomaticLocal(0);
    }
    scope.setAutomaticLocal(1, scope.getAutomaticLocal(0)->multiply(&scope, b));
    const PO* r = a->subtract(&scope, scope.getAutomaticLocal(1));
    scope.setAutomaticLocal(1, r);
    const PO* zero = scope.fromLong(0);
    if (kind == DivKind::Mod && numberCompare(&scope, r, zero) != 0 &&
        ((numberCompare(&scope, r, zero) < 0) != (numberCompare(&scope, b, zero) < 0)))
        r = r->add(&scope, b);
    scope.returnValue = r;
    return r;
}

COREPRIM(prim_quot) { return intDivision(ctx, args, DivKind::Quot, "quot"); }
COREPRIM(prim_rem)  { return intDivision(ctx, args, DivKind::Rem, "rem"); }
COREPRIM(prim_mod)  { return intDivision(ctx, args, DivKind::Mod, "mod"); }

// ---- compare -----------------------------------------------------------------

[[noreturn]] void notComparable(proto::ProtoContext* ctx, const PO* a, const PO* b) {
    throwClassed(ctx, "ClassCastException",
                 std::string("compare: ") + valueTypeName(ctx, a) + " cannot be compared with " +
                 valueTypeName(ctx, b));
}

// Clojure's compare: nil first; numbers by value across types; strings,
// keywords and symbols by their characters (keywords and symbols by spelling,
// so a qualified one sorts by its namespace first); booleans false before
// true; vectors by length, then element by element. Anything else, or two
// values of different kinds, raises ClassCastException.
int compareValues(proto::ProtoContext* ctx, const PO* a, const PO* b) {
    if (!a) a = PROTO_NONE;
    if (!b) b = PROTO_NONE;
    if (a == b) return 0;
    if (a == PROTO_NONE) return -1;
    if (b == PROTO_NONE) return 1;
    if (isNumber(ctx, a) && isNumber(ctx, b)) return numberCompare(ctx, a, b);
    const bool aStr = proto::ProtoObject::isStringTagFast(a);
    const bool bStr = proto::ProtoObject::isStringTagFast(b);
    if (aStr && bStr) {
        const std::string x = reinterpret_cast<const proto::ProtoString*>(a)->toStdString(ctx);
        const std::string y = reinterpret_cast<const proto::ProtoString*>(b)->toStdString(ctx);
        const int c = x.compare(y);
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
    if ((a == PROTO_TRUE || a == PROTO_FALSE) && (b == PROTO_TRUE || b == PROTO_FALSE))
        return a == PROTO_FALSE ? -1 : 1;
    const ActiveCallContext* cc = activeCallContext();
    if (cc && isNamed(ctx, cc->named, a) && isNamed(ctx, cc->named, b)) {
        const std::string x = namedSpelling(ctx, cc->named, a)->toStdString(ctx);
        const std::string y = namedSpelling(ctx, cc->named, b)->toStdString(ctx);
        const bool kx = !x.empty() && x[0] == ':', ky = !y.empty() && y[0] == ':';
        if (kx != ky) notComparable(ctx, a, b);
        const int c = x.compare(y);
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
    if (isVector(a) && isVector(b)) {
        const proto::ProtoList* x = vectorItems(ctx, a);
        const proto::ProtoList* y = vectorItems(ctx, b);
        const proto::proto_ulong nx = x->getSize(ctx), ny = y->getSize(ctx);
        if (nx != ny) return nx < ny ? -1 : 1;
        for (proto::proto_ulong i = 0; i < nx; ++i) {
            const int c = compareValues(ctx, x->getAt(ctx, static_cast<int>(i)),
                                        y->getAt(ctx, static_cast<int>(i)));
            if (c != 0) return c;
        }
        return 0;
    }
    notComparable(ctx, a, b);
}

COREPRIM(prim_compare) {
    if (argc(ctx, args) != 2) arity(ctx, argc(ctx, args), "compare");
    return ctx->fromLong(compareValues(ctx, argAt(ctx, args, 0), argAt(ctx, args, 1)));
}

// ---- max / min ------------------------------------------------------------------

const PO* extremum(proto::ProtoContext* ctx, const proto::ProtoArguments* args, bool wantMax,
                   const char* name) {
    const proto::proto_ulong n = argc(ctx, args);
    if (n == 0) arity(ctx, n, name);
    const PO* best = argAt(ctx, args, 0);
    if (!isNumber(ctx, best)) throwNotANumber(ctx, name, best);
    for (proto::proto_ulong i = 1; i < n; ++i) {
        const PO* x = argAt(ctx, args, i);
        if (!isNumber(ctx, x)) throwNotANumber(ctx, name, x);
        // A NaN wins, as in Clojure (its max and min propagate NaN).
        if (x->isFloat(ctx) && std::isnan(x->asDouble(ctx))) return x;
        const int c = numberCompare(ctx, x, best);
        if (wantMax ? c > 0 : c < 0) best = x;
    }
    return best;
}

COREPRIM(prim_max) { return extremum(ctx, args, true, "max"); }
COREPRIM(prim_min) { return extremum(ctx, args, false, "min"); }

// ---- sort / sort-by -----------------------------------------------------------

// A comparator as Clojure's sort uses one: nil is `compare`; a fn answering a
// number is a comparator; a fn answering a boolean is a "less than" predicate
// (AFunction.compare: true is -1, else (f y x) true is 1, else 0).
int callComparator(proto::ProtoContext* ctx, const PO* cmp, const PO* x, const PO* y) {
    if (!cmp || cmp == PROTO_NONE) return compareValues(ctx, x, y);
    const ActiveCallContext* cc = callContext(ctx, "sort");
    const PO* xy[2] = {x, y};
    proto::ProtoContext scope(ctx->space, ctx);
    const PO* r = cc->engine->invoke(&scope, cmp, xy, 2);
    if (r == PROTO_TRUE) return -1;
    if (!r || r == PROTO_NONE || r == PROTO_FALSE) {
        const PO* yx[2] = {y, x};
        return truthy(cc->engine->invoke(&scope, cmp, yx, 2)) ? 1 : 0;
    }
    if (!isNumber(&scope, r)) throwNotANumber(&scope, "sort comparator", r);
    return numberCompare(&scope, r, scope.fromLong(0));
}

// The elements of `coll` sorted by `keys` (the same elements when keyfn is
// nil) with `cmp`, stably, as Clojure's sort (a merge sort) is. The elements
// and the keys are rooted in lists in slots of `scope`.
const PO* sortedList(proto::ProtoContext* ctx, const PO* keyfn, const PO* cmp, const PO* coll) {
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(2);
    const proto::ProtoList* items = seqToList(&scope, coll);
    if (!items) return scope.newList()->asObject(&scope);
    scope.setAutomaticLocal(0, items->asObject(&scope));
    const proto::proto_ulong n = items->getSize(&scope);
    std::vector<const PO*> elems(n), keys;
    for (proto::proto_ulong i = 0; i < n; ++i) elems[i] = items->getAt(&scope, static_cast<int>(i));
    if (keyfn && keyfn != PROTO_NONE) {
        const ActiveCallContext* cc = callContext(&scope, "sort-by");
        // The keys are appended to a list rooted in slot 1 as they are made.
        scope.setAutomaticLocal(1, scope.newList()->asObject(&scope));
        for (proto::proto_ulong i = 0; i < n; ++i) {
            const PO* one[1] = {elems[i]};
            const PO* k = cc->engine->invoke(&scope, keyfn, one, 1);
            scope.setAutomaticLocal(1, scope.getAutomaticLocal(1)->asList(&scope)
                                           ->appendLast(&scope, k)->asObject(&scope));
        }
        const proto::ProtoList* kl = scope.getAutomaticLocal(1)->asList(&scope);
        keys.resize(n);
        for (proto::proto_ulong i = 0; i < n; ++i) keys[i] = kl->getAt(&scope, static_cast<int>(i));
    }
    std::vector<std::size_t> order(n);
    for (std::size_t i = 0; i < n; ++i) order[i] = i;
    const std::vector<const PO*>& by = keys.empty() ? elems : keys;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t i, std::size_t j) {
        return callComparator(&scope, cmp, by[i], by[j]) < 0;
    });
    std::vector<const PO*> out(n);
    for (std::size_t i = 0; i < n; ++i) out[i] = elems[order[i]];
    const PO* r = scope.newList(static_cast<unsigned>(n), out.data())->asObject(&scope);
    scope.returnValue = r;
    return r;
}

// (sort coll) / (sort comp coll): a seq of the elements, in order.
COREPRIM(prim_sort) {
    const proto::proto_ulong n = argc(ctx, args);
    if (n == 1) return sortedList(ctx, nullptr, nullptr, argAt(ctx, args, 0));
    if (n == 2) return sortedList(ctx, nullptr, argAt(ctx, args, 0), argAt(ctx, args, 1));
    arity(ctx, n, "sort");
}

// (sort-by keyfn coll) / (sort-by keyfn comp coll).
COREPRIM(prim_sort_by) {
    const proto::proto_ulong n = argc(ctx, args);
    if (n == 2) return sortedList(ctx, argAt(ctx, args, 0), nullptr, argAt(ctx, args, 1));
    if (n == 3) return sortedList(ctx, argAt(ctx, args, 0), argAt(ctx, args, 1), argAt(ctx, args, 2));
    arity(ctx, n, "sort-by");
}

// ---- frequencies ------------------------------------------------------------------

// (frequencies coll): a map from each distinct element to how many times it
// occurs. One pass with a C++ hash table keyed by the map key semantics
// (keyHash / keyEquals, MapOps.h); each new distinct element is appended to
// a list in a slot, which keeps it alive however the walk releases the
// collection, and the map is built at the end.
COREPRIM(prim_frequencies) {
    if (argc(ctx, args) != 1) arity(ctx, argc(ctx, args), "frequencies");
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(2);
    scope.setAutomaticLocal(1, scope.newList()->asObject(&scope));
    struct Hash {
        proto::ProtoContext* c;
        std::size_t operator()(const PO* k) const { return static_cast<std::size_t>(keyHash(c, k)); }
    };
    struct Eq {
        proto::ProtoContext* c;
        bool operator()(const PO* a, const PO* b) const { return keyEquals(c, a, b); }
    };
    std::unordered_map<const PO*, long long, Hash, Eq> counts(16, Hash{&scope}, Eq{&scope});
    std::vector<const PO*> distinct;
    {
        SeqWalk w(&scope, 0, &scope, argAt(ctx, args, 0));
        releaseArg(args, 0);
        for (; !w.done(); w.advance(&scope)) {
            const PO* x = w.first(&scope);
            auto it = counts.find(x);
            if (it != counts.end()) { ++it->second; continue; }
            counts.emplace(x, 1);
            distinct.push_back(x);
            scope.setAutomaticLocal(1, scope.getAutomaticLocal(1)->asList(&scope)
                                           ->appendLast(&scope, x)->asObject(&scope));
        }
    }
    std::vector<const PO*> kv;
    kv.reserve(2 * distinct.size());
    for (const PO* k : distinct) {
        kv.push_back(k);
        kv.push_back(scope.fromLong(counts.find(k)->second));
    }
    const PO* m = mapAssocPairs(&scope, nullptr, kv.data(), kv.size());
    scope.returnValue = m;
    return m;
}

// ---- clojure.string/split-lines --------------------------------------------------

// (split-lines s): the lines of `s` (split on \n or \r\n) as a vector;
// trailing empty lines are dropped, as Java's String.split drops them.
COREPRIM(prim_split_lines) {
    if (argc(ctx, args) != 1) arity(ctx, argc(ctx, args), "split-lines");
    const PO* v = argAt(ctx, args, 0);
    if (!proto::ProtoObject::isStringTagFast(v))
        throwClassed(ctx, "ClassCastException",
                     std::string("split-lines expects a string, got ") + valueTypeName(ctx, v));
    const std::string s = reinterpret_cast<const proto::ProtoString*>(v)->toStdString(ctx);
    std::vector<std::string> lines;
    std::size_t start = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n') {
            std::size_t end = i;
            if (end > start && s[end - 1] == '\r') --end;
            lines.push_back(s.substr(start, end - start));
            start = i + 1;
        }
    }
    lines.push_back(s.substr(start));
    while (lines.size() > 1 && lines.back().empty()) lines.pop_back();
    if (lines.size() == 1 && lines[0].empty() && !s.empty()) lines.clear();
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(1);
    scope.setAutomaticLocal(0, scope.newList()->asObject(&scope));
    for (const std::string& l : lines)
        scope.setAutomaticLocal(0, scope.getAutomaticLocal(0)->asList(&scope)
            ->appendLast(&scope, makeUtf8String(&scope, l))->asObject(&scope));
    const PO* r = newVector(&scope, scope.getAutomaticLocal(0)->asList(&scope));
    scope.returnValue = r;
    return r;
}

// ---- require -----------------------------------------------------------------

std::string symbolText(proto::ProtoContext* ctx, const ActiveCallContext* cc, const PO* v) {
    if (!cc || !isNamed(ctx, cc->named, v)) return "";
    return namedSpelling(ctx, cc->named, v)->toStdString(ctx);
}

[[noreturn]] void cannotLocate(proto::ProtoContext* ctx, const std::string& ns) {
    std::string path = ns;
    std::replace(path.begin(), path.end(), '.', '/');
    std::replace(path.begin(), path.end(), '-', '_');
    throwClassed(ctx, "FileNotFoundException",
                 "Could not locate " + path + "__init.class, " + path + ".clj or " + path +
                 ".cljc on classpath. (protoClojure provides clojure.core, clojure.string, "
                 "clojure.set, clojure.java.io, clojure.java.shell, clojure.edn, cheshire.core "
                 "and clojure.data.json; D52)");
}

// (require 'ns), (require '[ns :as alias :refer [...]]) ...: there are no
// namespaces to load (D28); a library namespace protoClojure provides is
// accepted and its `:as` alias recorded for the forms compiled afterwards
// (Macros.h, M33). `:refer` names are globals already. Any other namespace
// raises FileNotFoundException, as JVM Clojure does when it cannot find one.
COREPRIM(prim_require) {
    const ActiveCallContext* cc = callContext(ctx, "require");
    const proto::proto_ulong n = argc(ctx, args);
    for (proto::proto_ulong i = 0; i < n; ++i) {
        const PO* spec = argAt(ctx, args, i);
        std::string ns = symbolText(ctx, cc, spec);
        if (!ns.empty()) {
            if (ns[0] == ':') continue;  // :reload, :verbose ...
            if (!isKnownNamespace(ns)) cannotLocate(ctx, ns);
            continue;
        }
        const proto::ProtoList* parts = (isVector(spec) || isSeqValue(ctx, spec)) ? seqToList(ctx, spec) : nullptr;
        if (!parts || parts->getSize(ctx) == 0)
            throwClassed(ctx, "IllegalArgumentException",
                         "require: a spec is a symbol or [namespace & options], got " +
                         std::string(valueTypeName(ctx, spec)));
        ns = symbolText(ctx, cc, parts->getAt(ctx, 0));
        if (ns.empty() || ns[0] == ':')
            throwClassed(ctx, "IllegalArgumentException", "require: a spec starts with a namespace symbol");
        if (!isKnownNamespace(ns)) cannotLocate(ctx, ns);
        const proto::proto_ulong pn = parts->getSize(ctx);
        for (proto::proto_ulong j = 1; j + 1 < pn; j += 2) {
            const std::string opt = symbolText(ctx, cc, parts->getAt(ctx, static_cast<int>(j)));
            if (opt == ":as" || opt == ":as-alias") {
                const std::string alias = symbolText(ctx, cc, parts->getAt(ctx, static_cast<int>(j + 1)));
                if (alias.empty() || alias[0] == ':')
                    throwClassed(ctx, "IllegalArgumentException", "require: :as takes a symbol");
                addNamespaceAlias(alias, ns);
            } else if (opt != ":refer" && opt != ":rename") {
                throwClassed(ctx, "IllegalArgumentException", "require: unsupported option " + opt);
            }
        }
    }
    return PROTO_NONE;
}

// ---- parse-long / parse-double / parse-boolean --------------------------------

// The string argument of a parse-* function; Clojure raises
// IllegalArgumentException for anything else.
std::string parseArg(proto::ProtoContext* ctx, const proto::ProtoArguments* args, const char* who) {
    if (argc(ctx, args) != 1) arity(ctx, argc(ctx, args), who);
    const PO* v = argAt(ctx, args, 0);
    if (v == PROTO_NONE || !proto::ProtoObject::isStringTagFast(v))
        throwClassed(ctx, "IllegalArgumentException",
                     std::string("Expected string, got ") + valueTypeName(ctx, v));
    return reinterpret_cast<const proto::ProtoString*>(v)->toStdString(ctx);
}

// (parse-long s): the integer s spells (an optional sign and decimal
// digits, Java's Long.valueOf), nil when it spells none or is beyond 64 bits.
COREPRIM(prim_parse_long) {
    const std::string s = parseArg(ctx, args, "parse-long");
    std::size_t i = (!s.empty() && (s[0] == '+' || s[0] == '-')) ? 1 : 0;
    if (i == s.size()) return PROTO_NONE;
    for (std::size_t j = i; j < s.size(); ++j)
        if (s[j] < '0' || s[j] > '9') return PROTO_NONE;
    long long v = 0;
    const char* b = s.data() + (s[0] == '+' ? 1 : 0);
    const auto r = std::from_chars(b, s.data() + s.size(), v);
    if (r.ec != std::errc() || r.ptr != s.data() + s.size()) return PROTO_NONE;
    return ctx->fromLong(v);
}

// (parse-double s): the double s spells, as Java's Double.valueOf reads it
// (surrounding whitespace, a sign, digits with an optional fraction and
// exponent, `NaN`, `Infinity`), nil otherwise. Hexadecimal floats and the
// `d` / `f` suffixes Java also accepts are not (D58).
COREPRIM(prim_parse_double) {
    std::string s = parseArg(ctx, args, "parse-double");
    const auto notSpace = [](char c) { return c > ' '; };
    const auto b = std::find_if(s.begin(), s.end(), notSpace);
    const auto e = std::find_if(s.rbegin(), s.rend(), notSpace).base();
    s = b < e ? std::string(b, e) : std::string();
    bool neg = false;
    std::size_t i = 0;
    if (!s.empty() && (s[0] == '+' || s[0] == '-')) { neg = s[0] == '-'; i = 1; }
    const std::string body = s.substr(i);
    if (body == "NaN") return ctx->fromDouble(std::nan(""));
    if (body == "Infinity") return ctx->fromDouble(neg ? -HUGE_VAL : HUGE_VAL);
    // Digits, an optional fraction, an optional exponent; at least one digit
    // before or after the point.
    std::size_t k = 0, digits = 0;
    while (k < body.size() && std::isdigit(static_cast<unsigned char>(body[k]))) { ++k; ++digits; }
    if (k < body.size() && body[k] == '.') {
        ++k;
        while (k < body.size() && std::isdigit(static_cast<unsigned char>(body[k]))) { ++k; ++digits; }
    }
    if (digits == 0) return PROTO_NONE;
    if (k < body.size() && (body[k] == 'e' || body[k] == 'E')) {
        ++k;
        if (k < body.size() && (body[k] == '+' || body[k] == '-')) ++k;
        std::size_t exp = 0;
        while (k < body.size() && std::isdigit(static_cast<unsigned char>(body[k]))) { ++k; ++exp; }
        if (exp == 0) return PROTO_NONE;
    }
    if (k != body.size()) return PROTO_NONE;
    // strtod, as the lexer reads float literals (portable where
    // std::from_chars for doubles is not); overflow is an infinity.
    const double d = std::strtod(body.c_str(), nullptr);
    return ctx->fromDouble(neg ? -d : d);
}

// (parse-boolean s): true for "true", false for "false", nil otherwise.
COREPRIM(prim_parse_boolean) {
    const std::string s = parseArg(ctx, args, "parse-boolean");
    if (s == "true") return PROTO_TRUE;
    if (s == "false") return PROTO_FALSE;
    return PROTO_NONE;
}

// ---- bigint / biginteger ---------------------------------------------------------

// (bigint x), (biginteger x): x as an exact integer. Integers are not a
// separate type below and above 64 bits (D14), so an integer answers itself;
// a float is truncated toward zero from its shortest decimal digits, as
// Clojure does through BigDecimal.valueOf ((bigint 1e30) is 10^30); a
// string of decimal digits (with an optional sign) is parsed, as Java's
// BigInteger(String) parses it.
COREPRIM(prim_bigint) {
    if (argc(ctx, args) != 1) arity(ctx, argc(ctx, args), "bigint");
    const PO* v = argAt(ctx, args, 0);
    if (v != PROTO_NONE && v->isInteger(ctx)) return v;
    if (v != PROTO_NONE && v->isFloat(ctx)) {
        // As Clojure: BigDecimal.valueOf(x), which reads the shortest
        // decimal digits of x (Double.toString, formatDouble), truncated
        // toward zero. So (bigint 1e30) is 10^30, not the double's exact
        // binary value.
        const double d = v->asDouble(ctx);
        if (!std::isfinite(d)) throwClassed(ctx, "NumberFormatException", "Infinite or NaN");
        const std::string text = formatDouble(d);  // "-1.0E30", "123.45"
        std::size_t i = 0;
        const bool neg = text[0] == '-';
        if (neg) i = 1;
        std::string digits;
        long long point = 0;  // digits before the decimal point
        for (; i < text.size() && text[i] != 'E'; ++i) {
            if (text[i] == '.') point = static_cast<long long>(digits.size());
            else digits += text[i];
        }
        if (i < text.size()) point += std::stoll(text.substr(i + 1));
        std::string whole;
        if (point > 0) {
            whole = digits.substr(0, static_cast<std::size_t>(std::min<long long>(point, digits.size())));
            if (point > static_cast<long long>(digits.size()))
                whole.append(static_cast<std::size_t>(point) - digits.size(), '0');
        }
        const std::size_t nz = whole.find_first_not_of('0');
        if (nz == std::string::npos) return ctx->fromLong(0);
        whole = (neg ? "-" : "") + whole.substr(nz);
        return ctx->fromString(whole.c_str(), 10);
    }
    if (v != PROTO_NONE && proto::ProtoObject::isStringTagFast(v)) {
        const std::string s = reinterpret_cast<const proto::ProtoString*>(v)->toStdString(ctx);
        std::size_t i = (!s.empty() && (s[0] == '+' || s[0] == '-')) ? 1 : 0;
        bool ok = i < s.size();
        for (std::size_t j = i; ok && j < s.size(); ++j) ok = s[j] >= '0' && s[j] <= '9';
        if (!ok) throwClassed(ctx, "NumberFormatException", "For input string: \"" + s + "\"");
        return ctx->fromString(s[0] == '+' ? s.c_str() + 1 : s.c_str(), 10);
    }
    throwClassed(ctx, "ClassCastException",
                 std::string("bigint expects a number or a string, got ") + valueTypeName(ctx, v));
}

// ---- number predicates, ==, hash, peek, pop ------------------------------------

bool isListValue(const PO* v) {
    constexpr proto::proto_ulong kTagMask = 0x3F, kTagList = 2, kTagListSmall = 25;
    const proto::proto_ulong tag = reinterpret_cast<proto::proto_ulong>(v) & kTagMask;
    return v != nullptr && (tag == kTagList || tag == kTagListSmall);
}

const PO* oneArg(proto::ProtoContext* ctx, const proto::ProtoArguments* args, const char* who) {
    if (argc(ctx, args) != 1) arity(ctx, argc(ctx, args), who);
    return argAt(ctx, args, 0);
}

// (number? x), (integer? x) / (int? x), (float? x) / (double? x). Integers
// are one type at every size (D14), so int? (JVM Clojure: a fixed-width
// integer) answers true for a big integer too.
COREPRIM(prim_number_p)  { return isNumber(ctx, oneArg(ctx, args, "number?")) ? PROTO_TRUE : PROTO_FALSE; }
COREPRIM(prim_integer_p) {
    const PO* v = oneArg(ctx, args, "integer?");
    return v != PROTO_NONE && isNumber(ctx, v) && v->isInteger(ctx) ? PROTO_TRUE : PROTO_FALSE;
}
COREPRIM(prim_float_p) {
    const PO* v = oneArg(ctx, args, "float?");
    return v != PROTO_NONE && isNumber(ctx, v) && v->isFloat(ctx) ? PROTO_TRUE : PROTO_FALSE;
}

// (== x y & more): numeric equality across number types; a non-number
// raises ClassCastException, as in Clojure.
COREPRIM(prim_num_eq) {
    const proto::proto_ulong n = argc(ctx, args);
    if (n < 1) arity(ctx, n, "==");
    for (proto::proto_ulong i = 0; i < n; ++i)
        if (!isNumber(ctx, argAt(ctx, args, i))) throwNotANumber(ctx, "==", argAt(ctx, args, i));
    for (proto::proto_ulong i = 1; i < n; ++i) {
        const PO* a = argAt(ctx, args, i - 1);
        const PO* b = argAt(ctx, args, i);
        if ((a->isFloat(ctx) && std::isnan(a->asDouble(ctx))) ||
            (b->isFloat(ctx) && std::isnan(b->asDouble(ctx))))
            return PROTO_FALSE;
        if (numberCompare(ctx, a, b) != 0) return PROTO_FALSE;
    }
    return PROTO_TRUE;
}

// (hash x): the hash map keys use (MapOps.h keyHash), consistent with how
// maps and sets match keys; the value differs from JVM Clojure's (D59).
COREPRIM(prim_hash) {
    const PO* v = oneArg(ctx, args, "hash");
    const proto::proto_ulong h = keyHash(ctx, v);
    return ctx->fromLong(static_cast<long long>(h & 0xFFFFFFFFull) - ((h & 0x80000000ull) ? (1ll << 32) : 0));
}

// (peek coll): the last element of a vector, the first of a list; nil for
// nil or an empty collection.
COREPRIM(prim_peek) {
    const PO* v = oneArg(ctx, args, "peek");
    if (v == PROTO_NONE) return PROTO_NONE;
    if (isVector(v)) {
        const proto::ProtoList* items = vectorItems(ctx, v);
        return items->getSize(ctx) == 0 ? PROTO_NONE : items->getLast(ctx);
    }
    if (isListValue(v)) {
        const proto::ProtoList* l = v->asList(ctx);
        return l->getSize(ctx) == 0 ? PROTO_NONE : l->getFirst(ctx);
    }
    throwClassed(ctx, "ClassCastException",
                 std::string("peek expects a vector or a list, got ") + valueTypeName(ctx, v));
}

// (pop coll): a vector without its last element, a list without its first;
// nil for nil; IllegalStateException for an empty one, as in Clojure.
COREPRIM(prim_pop) {
    const PO* v = oneArg(ctx, args, "pop");
    if (v == PROTO_NONE) return PROTO_NONE;
    if (isVector(v)) {
        const proto::ProtoList* items = vectorItems(ctx, v);
        if (items->getSize(ctx) == 0) throwClassed(ctx, "IllegalStateException", "Can't pop empty vector");
        proto::ProtoContext scope(ctx->space, ctx);
        scope.resizeAutomaticLocals(1);
        scope.setAutomaticLocal(0, items->removeLast(&scope)->asObject(&scope));
        const PO* r = newVector(&scope, scope.getAutomaticLocal(0)->asList(&scope));
        scope.returnValue = r;
        return r;
    }
    if (isListValue(v)) {
        const proto::ProtoList* l = v->asList(ctx);
        if (l->getSize(ctx) == 0) throwClassed(ctx, "IllegalStateException", "Can't pop empty list");
        return l->removeFirst(ctx)->asObject(ctx);
    }
    throwClassed(ctx, "ClassCastException",
                 std::string("pop expects a vector or a list, got ") + valueTypeName(ctx, v));
}

// ---- System ----------------------------------------------------------------------

COREPRIM(prim_current_time_millis) {
    if (argc(ctx, args) != 0) arity(ctx, argc(ctx, args), "System/currentTimeMillis");
    using namespace std::chrono;
    return ctx->fromLong(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

COREPRIM(prim_nano_time) {
    if (argc(ctx, args) != 0) arity(ctx, argc(ctx, args), "System/nanoTime");
    using namespace std::chrono;
    return ctx->fromLong(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

#undef COREPRIM

struct CorePrim { const char* name; proto::ProtoMethod fn; };
constexpr CorePrim kCorePrims[] = {
    {"quot",        &prim_quot},
    {"rem",         &prim_rem},
    {"mod",         &prim_mod},
    {"compare",     &prim_compare},
    {"max",         &prim_max},
    {"min",         &prim_min},
    {"sort",        &prim_sort},
    {"sort-by",     &prim_sort_by},
    {"frequencies", &prim_frequencies},
    {"split-lines", &prim_split_lines},
    {"require",     &prim_require},
    {"parse-long",    &prim_parse_long},
    {"parse-double",  &prim_parse_double},
    {"parse-boolean", &prim_parse_boolean},
    {"bigint",        &prim_bigint},
    {"number?",       &prim_number_p},
    {"integer?",      &prim_integer_p},
    {"int?",          &prim_integer_p},
    {"float?",        &prim_float_p},
    {"double?",       &prim_float_p},
    {"==",            &prim_num_eq},
    {"hash",          &prim_hash},
    {"peek",          &prim_peek},
    {"pop",           &prim_pop},
    {"biginteger",    &prim_bigint},
};

} // namespace

void installCorePrimitives(proto::ProtoContext* ctx, proto::ProtoObject* globals) {
    for (const CorePrim& p : kCorePrims)
        defineBuiltin(ctx, globals, p.name, ctx->fromMethod(nullptr, p.fn));
    // `+'`, `-'`, `*'`, `inc'` and `dec'`: Clojure's auto-promoting
    // arithmetic. protoClojure's `+ - * inc dec` already promote to exact big
    // integers (D14), so the primed names are the same functions.
    for (const char* op : {"+", "-", "*", "inc", "dec"}) {
        const PO* f = globals->getOwnAttributeDirect(ctx, proto::ProtoString::createSymbol(ctx, op));
        if (f) defineBuiltin(ctx, globals, (std::string(op) + "'").c_str(), f);
    }
    // `System`, an object whose members stand in for java.lang.System's
    // static methods a script calls: (System/exit n) is `exit`,
    // (System/getenv "X") `getenv`. Installed after the I/O globals.
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(1);
    scope.setAutomaticLocal(0, ctx->space->objectPrototype->newChild(&scope, false));
    auto member = [&](const char* name, const PO* value) {
        if (!value) return;
        scope.setAutomaticLocal(0, scope.getAutomaticLocal(0)->setAttribute(
            &scope, proto::ProtoString::createSymbol(&scope, name), value));
    };
    member("exit", globals->getOwnAttributeDirect(&scope, proto::ProtoString::createSymbol(&scope, "exit")));
    member("getenv", globals->getOwnAttributeDirect(&scope, proto::ProtoString::createSymbol(&scope, "getenv")));
    member("currentTimeMillis", scope.fromMethod(nullptr, &prim_current_time_millis));
    member("nanoTime", scope.fromMethod(nullptr, &prim_nano_time));
    defineBuiltin(&scope, globals, "System", scope.getAutomaticLocal(0));
}

const char* corePrimitiveName(proto::ProtoMethod fn) {
    for (const CorePrim& p : kCorePrims)
        if (p.fn == fn) return p.name;
    return nullptr;
}

} // namespace protoClojure
