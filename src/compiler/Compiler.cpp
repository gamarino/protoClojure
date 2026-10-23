#include "Compiler.h"

#include "runtime/Exceptions.h"
#include "runtime/Macros.h"
#include "runtime/MapOps.h"
#include "runtime/SetOps.h"
#include "runtime/Primitives.h"
#include "runtime/StackGuard.h"
#include "runtime/VectorOps.h"

#include "protoCore.h"

#include <cstdio>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace protoClojure {

namespace {

// Pointer-tag constants matching protoCore's internal layout. Defined here
// to avoid pulling proto_internal.h; copied from headers/proto_internal.h
// and stable across protoCore versions (they are baked into every cell).
constexpr unsigned int kTagList      = 2;   // POINTER_TAG_LIST
constexpr unsigned int kTagListSmall = 25;  // POINTER_TAG_LIST_SMALL

unsigned int tagOf(const proto::ProtoObject* form) {
    return static_cast<unsigned int>(
        reinterpret_cast<uintptr_t>(form) & 0x3F);
}

// True iff the form is a genuine ProtoList — large or small inline form.
// We do NOT use `asList(ctx)` here because it treats a ProtoString as a
// sequence of characters and returns a non-null ProtoList, which would
// make the compiler walk every char of every string literal as if it
// were a call form. The tag check is precise.
bool isList(const proto::ProtoObject* form) {
    if (!form) return false;
    unsigned int t = tagOf(form);
    return t == kTagList || t == kTagListSmall;
}

// True iff the form is a symbol or a keyword: a named value
// (runtime/Named.h), which is what the reader yields for both. The compiler
// tells the two apart by spelling where it matters (a keyword's starts with
// `:`), and by the form's position otherwise.
bool isNameForm(proto::ProtoContext* ctx, const NamedLayout& named,
                const proto::ProtoObject* form) {
    return isNamed(ctx, named, form);
}

// The spelling of a symbol or keyword form (`isNameForm` must hold), with
// the leading `:` of a keyword. The std::string lives only on the C++ stack
// (P3).
std::string nameOf(proto::ProtoContext* ctx, const NamedLayout& named,
                   const proto::ProtoObject* form) {
    return namedSpelling(ctx, named, form)->toStdString(ctx);
}

// Strips a leading `clojure.core/` from `name` and returns true when there
// was one. Without namespaces (D28) the built-ins are the globals the
// runtime installs, so `clojure.core/x` names the global x; unlike a bare
// `x` it is never resolved to a local. Syntax-quote and the reader macros
// `@`, `~` and `~@` produce such names, as Clojure's reader does.
bool stripCoreQualifier(std::string& name) {
    static const std::string kCore = "clojure.core/";
    if (name.size() <= kCore.size() || name.compare(0, kCore.size(), kCore) != 0) return false;
    name.erase(0, kCore.size());
    return true;
}

// Stores the value on top of the stack in the global `key`, leaving it
// there (STORE_GLOBAL does). While core.clj loads it is also stored under
// `clojure.core/key`, the built-in's own key (Macros.h).
void emitStoreGlobal(proto::ProtoContext* ctx, const std::string& key, BytecodeModule& out) {
    out.emit(Op::STORE_GLOBAL, out.addSymbol(ctx, key));
    if (coreNamespaceActive())
        out.emit(Op::STORE_GLOBAL, out.addSymbol(ctx, "clojure.core/" + key));
}

// True for `user/x`: a symbol syntax-quote qualified with the program's
// namespace (there is only `user`, D28); it names the global x.
bool isUserQualified(const std::string& name) {
    return name.size() > 5 && name.compare(0, 5, "user/") == 0;
}

// True iff the form is a string literal: a protoCore string. Symbols and
// keywords are named values, never strings.
bool isStringForm(const proto::ProtoObject* form) {
    return form && form != PROTO_NONE && proto::ProtoObject::isStringTagFast(form);
}

// The symbol `name` as a form, for the forms the compiler builds itself
// (the fn wrapper of a top-level form, defmacro's hidden parameters).
const proto::ProtoObject* symbolForm(proto::ProtoContext* ctx, const NamedLayout& named,
                                     const char* name) {
    return internNamed(ctx, named, name);
}

// The const-pool entry for the integer `value`, a literal or a quoted
// integer: a Long when it fits a long long, otherwise its decimal digits
// (ConstKind::BigInteger), so a literal beyond 64 bits keeps its exact value.
std::size_t addIntegerConst(proto::ProtoContext* ctx,
                            const proto::ProtoObject* value,
                            BytecodeModule& out) {
    try {
        return out.addLong(value->asLong(ctx));
    } catch (const proto::ProtoException&) {
        // protoCore's Overflow error: the integer does not fit a long long.
        return out.addBigInteger(value->asIntegerString(ctx, 10)->toStdString(ctx));
    }
}

// The list form of `items`, in order, for the forms the compiler builds
// itself. Allocated in `ctx`, whose young generation protects it (and the
// items) until the caller roots it.
const proto::ProtoObject* listForm(proto::ProtoContext* ctx,
                                   std::initializer_list<const proto::ProtoObject*> items) {
    const proto::ProtoList* l = ctx->newList();
    for (const proto::ProtoObject* item : items) l = l->appendLast(ctx, item);
    return l->asObject(ctx);
}

// A fresh generated symbol `prefixN`, as the gensyms clojure.core's
// destructure uses (`vec__12`, `map__13`).
const proto::ProtoObject* freshSymbol(proto::ProtoContext* ctx, const NamedLayout& named,
                                      const char* prefix) {
    const std::string s = prefix + std::to_string(nextGensymId());
    return internNamed(ctx, named, s.c_str());
}

// The form as `pr` prints it, for an error message.
std::string printedForm(proto::ProtoContext* ctx, const proto::ProtoObject* form) {
    std::string s;
    printValueTo(ctx, s, form, /*readable=*/true);
    return s;
}

} // namespace

// True iff `form` is a keyword: a named value spelled with a leading `:`.
// Used to detect a trailing kv-pair suffix at call sites.
static bool isKeywordForm(proto::ProtoContext* ctx, const NamedLayout& named,
                          const proto::ProtoObject* form) {
    if (!isNameForm(ctx, named, form)) return false;
    std::string s = nameOf(ctx, named, form);
    return !s.empty() && s[0] == ':';
}

// The entries of the map form `m` as alternating keys and values: in source
// order when the reader recorded it (SourceInfo.h), in the map's walk order
// otherwise. The map is rooted by the caller, and protoCore does not move
// objects, so the pointers stay valid while the map lives.
std::vector<const proto::ProtoObject*>
Compiler::mapFormEntries(proto::ProtoContext* ctx, const proto::ProtoObject* m) const {
    std::vector<const proto::ProtoObject*> kv;
    if (source_) {
        auto it = source_->mapKeyOrder.find(m);
        if (it != source_->mapKeyOrder.end()) {
            for (const proto::ProtoObject* k : it->second) {
                bool found = false;
                const proto::ProtoObject* v = mapGet(ctx, m, k, &found);
                if (!found) break;  // not the map the reader recorded
                kv.push_back(k);
                kv.push_back(v);
            }
            if (kv.size() == 2 * mapCount(ctx, m)) return kv;
            kv.clear();
        }
    }
    mapForEach(ctx, m, &kv,
        [](proto::ProtoContext*, void* self, const proto::ProtoObject* k,
           const proto::ProtoObject* v) {
            auto* out = static_cast<std::vector<const proto::ProtoObject*>*>(self);
            out->push_back(k);
            out->push_back(v);
        });
    return kv;
}

// Destructuring, following clojure.core/destructure (Clojure 1.11):
//
//   [a b & r :as all] v   vec__ v, all vec__, seq__ (seq vec__),
//                         first__ (first seq__), seq__ (next seq__), a first__,
//                         ... r seq__
//   [a b] v               vec__ v, a (nth vec__ 0 nil), b (nth vec__ 1 nil)
//   {a :a :keys [b] :strs [c] :syms [d] :or {b 1} :as m} v
//                         map__ v, map__ (__destructure-map map__), m map__,
//                         a (get map__ :a), b (get map__ :b 1),
//                         c (get map__ "c"), d (get map__ 'd)
//
// Nested forms recurse on the expression that reads their part. Generated
// names are rebound freely (`seq__` once per element): a `let` binding gets a
// fresh slot each time, so each read sees the binding before it.
void Compiler::destructure(proto::ProtoContext* ctx, const CompilerMarkers& markers,
                           const proto::ProtoObject* pattern, const proto::ProtoObject* value,
                           std::vector<const proto::ProtoObject*>& out) const {
    checkNativeStack(StackUse::Source);
    const NamedLayout& named = markers.named;
    auto sym = [&](const char* s) { return internNamed(ctx, named, s); };

    if (isNameForm(ctx, named, pattern)) {
        const std::string s = nameOf(ctx, named, pattern);
        if (!s.empty() && s[0] == ':')
            throw CompileError("Unsupported binding form: " + s);
        out.push_back(pattern);
        out.push_back(value);
        return;
    }

    if (isVector(pattern)) {
        const proto::ProtoList* items = vectorItems(ctx, pattern);
        const proto::proto_ulong n = items->getSize(ctx);
        const proto::ProtoObject* amp = sym("&");
        const proto::ProtoObject* asKw = sym(":as");
        bool hasRest = false;
        for (proto::proto_ulong i = 0; i < n; ++i)
            if (items->getAt(ctx, static_cast<int>(i)) == amp) hasRest = true;
        const proto::ProtoObject* gvec = freshSymbol(ctx, named, "vec__");
        out.push_back(gvec);
        out.push_back(value);
        const proto::ProtoObject* gseq = nullptr;
        if (hasRest) {
            gseq = freshSymbol(ctx, named, "seq__");
            out.push_back(gseq);
            out.push_back(listForm(ctx, {sym("clojure.core/seq"), gvec}));
        }
        bool seenRest = false;
        long long index = 0;
        for (proto::proto_ulong i = 0; i < n; ++i) {
            const proto::ProtoObject* b = items->getAt(ctx, static_cast<int>(i));
            if (b == amp || b == asKw) {
                if (i + 1 >= n)
                    throw CompileError(std::string("Unsupported binding form, missing binding after ") +
                                       (b == amp ? "&" : ":as"));
                const proto::ProtoObject* target = items->getAt(ctx, static_cast<int>(i + 1));
                if (b == amp) {
                    if (seenRest)
                        throw CompileError("Unsupported binding form, only :as can follow & parameter");
                    destructure(ctx, markers, target, gseq, out);
                    seenRest = true;
                    ++i;
                    continue;
                }
                // `:as` binds the whole value; Clojure ignores what follows.
                destructure(ctx, markers, target, gvec, out);
                return;
            }
            if (seenRest)
                throw CompileError("Unsupported binding form, only :as can follow & parameter");
            if (hasRest) {
                const proto::ProtoObject* gfirst = freshSymbol(ctx, named, "first__");
                out.push_back(gfirst);
                out.push_back(listForm(ctx, {sym("clojure.core/first"), gseq}));
                out.push_back(gseq);
                out.push_back(listForm(ctx, {sym("clojure.core/next"), gseq}));
                destructure(ctx, markers, b, gfirst, out);
            } else {
                destructure(ctx, markers, b,
                            listForm(ctx, {sym("clojure.core/nth"), gvec,
                                           ctx->fromLong(index), PROTO_NONE}),
                            out);
            }
            ++index;
        }
        return;
    }

    if (isMap(pattern)) {
        const std::vector<const proto::ProtoObject*> entries = mapFormEntries(ctx, pattern);
        const proto::ProtoObject* gmap = freshSymbol(ctx, named, "map__");
        out.push_back(gmap);
        out.push_back(value);
        out.push_back(gmap);
        out.push_back(listForm(ctx, {sym("clojure.core/__destructure-map"), gmap}));
        // :or defaults, by the name of the local they default.
        std::unordered_map<std::string, const proto::ProtoObject*> defaults;
        for (std::size_t j = 0; j < entries.size(); j += 2) {
            if (entries[j] != sym(":or")) continue;
            if (!isMap(entries[j + 1])) throw CompileError(":or must be a map, got " + printedForm(ctx, entries[j + 1]));
            const std::vector<const proto::ProtoObject*> ors = mapFormEntries(ctx, entries[j + 1]);
            for (std::size_t t = 0; t < ors.size(); t += 2) {
                if (!isNameForm(ctx, named, ors[t]))
                    throw CompileError(":or key must be a symbol, got " + printedForm(ctx, ors[t]));
                defaults[nameOf(ctx, named, ors[t])] = ors[t + 1];
            }
        }
        auto getForm = [&](const proto::ProtoObject* key, const std::string& local) {
            auto it = defaults.find(local);
            if (it == defaults.end()) return listForm(ctx, {sym("clojure.core/get"), gmap, key});
            return listForm(ctx, {sym("clojure.core/get"), gmap, key, it->second});
        };
        for (std::size_t j = 0; j < entries.size(); j += 2) {
            const proto::ProtoObject* k = entries[j];
            const proto::ProtoObject* v = entries[j + 1];
            if (k == sym(":or")) continue;
            if (k == sym(":as")) {
                if (!isNameForm(ctx, named, v) || isKeywordForm(ctx, named, v))
                    throw CompileError(":as must be followed by a symbol, got " + printedForm(ctx, v));
                out.push_back(v);
                out.push_back(gmap);
                continue;
            }
            if (isKeywordForm(ctx, named, k)) {
                // :keys, :strs, :syms, or :ns/keys and :ns/syms.
                const std::string kw = nameOf(ctx, named, k).substr(1);
                const std::size_t slash = kw.find('/');
                const std::string kind = slash == std::string::npos ? kw : kw.substr(slash + 1);
                const std::string kwNs = slash == std::string::npos ? "" : kw.substr(0, slash);
                if (kind != "keys" && kind != "strs" && kind != "syms")
                    throw CompileError("Unsupported binding key: " + nameOf(ctx, named, k));
                if (kind == "strs" && !kwNs.empty())
                    throw CompileError("Unsupported binding key: " + nameOf(ctx, named, k));
                if (!isVector(v))
                    throw CompileError(nameOf(ctx, named, k) + " must be followed by a vector, got " +
                                       printedForm(ctx, v));
                const proto::ProtoList* names = vectorItems(ctx, v);
                const proto::proto_ulong nn = names->getSize(ctx);
                for (proto::proto_ulong t = 0; t < nn; ++t) {
                    const proto::ProtoObject* s = names->getAt(ctx, static_cast<int>(t));
                    if (!isNameForm(ctx, named, s))
                        throw CompileError("Unsupported binding form: " + printedForm(ctx, s));
                    std::string spelled = nameOf(ctx, named, s);
                    if (spelled[0] == ':') spelled.erase(0, 1);  // `:keys [:a]`
                    const std::size_t sl = spelled.find('/');
                    const std::string local = sl == std::string::npos ? spelled : spelled.substr(sl + 1);
                    std::string full = spelled;  // the key's namespace/name
                    if (sl == std::string::npos && !kwNs.empty()) full = kwNs + "/" + spelled;
                    const proto::ProtoObject* key = nullptr;
                    if (kind == "keys") key = sym((":" + full).c_str());
                    else if (kind == "strs") key = ctx->fromUTF8String(local.c_str());
                    else key = listForm(ctx, {sym("quote"), sym(full.c_str())});
                    out.push_back(sym(local.c_str()));
                    out.push_back(getForm(key, local));
                }
                continue;
            }
            // {binding-form key}: the key is an expression, as in Clojure.
            if (isNameForm(ctx, named, k)) {
                out.push_back(k);
                out.push_back(getForm(v, nameOf(ctx, named, k)));
            } else {
                destructure(ctx, markers, k, listForm(ctx, {sym("clojure.core/get"), gmap, v}), out);
            }
        }
        return;
    }

    throw CompileError("Unsupported binding form: " + printedForm(ctx, pattern));
}

const proto::ProtoObject* Compiler::destructureBindings(proto::ProtoContext* ctx,
                                                        const CompilerMarkers& markers,
                                                        const proto::ProtoList* bindings) const {
    const proto::proto_ulong n = bindings->getSize(ctx);
    bool simple = true;
    for (proto::proto_ulong i = 0; i < n; i += 2) {
        const proto::ProtoObject* b = bindings->getAt(ctx, static_cast<int>(i));
        if (!isNameForm(ctx, markers.named, b) || isKeywordForm(ctx, markers.named, b)) simple = false;
    }
    if (simple) return nullptr;
    std::vector<const proto::ProtoObject*> flat;
    for (proto::proto_ulong i = 0; i + 1 < n; i += 2)
        destructure(ctx, markers, bindings->getAt(ctx, static_cast<int>(i)),
                    bindings->getAt(ctx, static_cast<int>(i + 1)), flat);
    const proto::ProtoList* l = ctx->newList();
    for (const proto::ProtoObject* f : flat) l = l->appendLast(ctx, f);
    return newVector(ctx, l);
}

std::unique_ptr<BytecodeModule>
Compiler::compileFnBody(proto::ProtoContext* ctx,
                        const proto::ProtoList* fnForm,
                        const CompilerMarkers& markers) {
    // Detect whether the form is (fn [params] body...) or
    // (defn name [params] body...). The compileForm dispatcher routes
    // both here and we discover the shape from the head's name.
    proto::proto_ulong n = fnForm->getSize(ctx);
    const proto::ProtoObject* head = fnForm->getAt(ctx, 0);
    std::string headName = nameOf(ctx, markers.named, head);
    stripCoreQualifier(headName);  // `clojure.core/fn`, from a syntax-quote
    proto::proto_ulong paramsAt = 0;
    if (headName == "fn")        paramsAt = 1;
    else if (headName == "defn") paramsAt = 2;
    else throw CompileError("compileFnBody: head must be `fn` or `defn`");

    if (n <= paramsAt) throw CompileError("fn/defn: missing parameter vector");
    const proto::ProtoObject* paramsForm = fnForm->getAt(ctx, (int)paramsAt);
    // Session 18 — accept `(fn name [params] body...)`: a symbol where
    // the params vector should be. The name itself is currently
    // dropped (self-reference via `name` inside the body is a v0.2
    // follow-up — needs a wrapper-into-slot capture). Shifting
    // paramsAt by one is enough to make the surface parse.
    if (headName == "fn" && isNameForm(ctx, markers.named, paramsForm)) {
        paramsAt += 1;
        if (n <= paramsAt) throw CompileError("fn: missing parameter vector after name");
        paramsForm = fnForm->getAt(ctx, (int)paramsAt);
    }
    const proto::ProtoList* params = nullptr;
    if (isVector(paramsForm)) {
        params = vectorItems(ctx, paramsForm);
    } else if (isList(paramsForm)) {
        params = paramsForm->asList(ctx);
    } else {
        throw CompileError("fn/defn: parameter list must be a vector");
    }
    return compileArity(ctx, params, fnForm, paramsAt + 1, markers);
}

std::unique_ptr<BytecodeModule>
Compiler::compileArity(proto::ProtoContext* ctx,
                       const proto::ProtoList* params,
                       const proto::ProtoList* arityForm,
                       proto::proto_ulong bodyStartIdx,
                       const CompilerMarkers& markers) {
    // Destructured parameters, as clojure.core/fn does it: each parameter
    // that is a vector or map form becomes a generated symbol p__N, and the
    // body is compiled as (let [form p__N ...] body...). `& [a b]` and a
    // `& {...}` map the keyword-argument path below cannot take (anything
    // but symbol :keys, :or and :as) become a variadic rest bound the same
    // way. The rewritten forms are rooted in this scope's slots.
    proto::ProtoContext dscope(ctx->space, ctx);
    dscope.resizeAutomaticLocals(2);
    ctx = &dscope;
    {
        const proto::proto_ulong pc = params->getSize(ctx);
        auto kwPathTakes = [&](const proto::ProtoObject* m) {
            const std::vector<const proto::ProtoObject*> es = mapFormEntries(ctx, m);
            bool keys = false;
            for (std::size_t j = 0; j < es.size(); j += 2) {
                if (!isNameForm(ctx, markers.named, es[j])) return false;
                const std::string k = nameOf(ctx, markers.named, es[j]);
                if (k == ":keys") {
                    if (!isVector(es[j + 1])) return false;
                    const proto::ProtoList* ks = vectorItems(ctx, es[j + 1]);
                    for (proto::proto_ulong t = 0; t < ks->getSize(ctx); ++t) {
                        const proto::ProtoObject* s = ks->getAt(ctx, static_cast<int>(t));
                        if (!isNameForm(ctx, markers.named, s) || isKeywordForm(ctx, markers.named, s) ||
                            isQualifiedName(nameOf(ctx, markers.named, s)))
                            return false;
                    }
                    keys = true;
                } else if (k == ":or") {
                    if (!isMap(es[j + 1])) return false;
                } else if (k == ":as") {
                    if (!isNameForm(ctx, markers.named, es[j + 1])) return false;
                } else {
                    return false;
                }
            }
            return keys;
        };
        std::vector<const proto::ProtoObject*> pairs;  // form, generated symbol
        const proto::ProtoList* effective = ctx->newList();
        for (proto::proto_ulong i = 0; i < pc; ++i) {
            const proto::ProtoObject* p = params->getAt(ctx, static_cast<int>(i));
            const bool afterAmp = i > 0 && isNameForm(ctx, markers.named, params->getAt(ctx, static_cast<int>(i - 1))) &&
                                  nameOf(ctx, markers.named, params->getAt(ctx, static_cast<int>(i - 1))) == "&";
            if (!isVector(p) && !isMap(p) &&
                (!isNameForm(ctx, markers.named, p) || isKeywordForm(ctx, markers.named, p)))
                throw CompileError("Unsupported binding form: " + printedForm(ctx, p));
            if (isVector(p) || (isMap(p) && !(afterAmp && kwPathTakes(p)))) {
                const proto::ProtoObject* g = freshSymbol(ctx, markers.named, "p__");
                pairs.push_back(p);
                pairs.push_back(g);
                effective = effective->appendLast(ctx, g);
            } else {
                effective = effective->appendLast(ctx, p);
            }
        }
        if (!pairs.empty()) {
            dscope.setAutomaticLocal(0, effective->asObject(ctx));
            params = dscope.getAutomaticLocal(0)->asList(ctx);
            const proto::ProtoList* bvec = ctx->newList();
            for (const proto::ProtoObject* f : pairs) bvec = bvec->appendLast(ctx, f);
            const proto::ProtoList* letForm = ctx->newList()
                ->appendLast(ctx, symbolForm(ctx, markers.named, "let"))
                ->appendLast(ctx, newVector(ctx, bvec));
            const proto::proto_ulong an = arityForm->getSize(ctx);
            for (proto::proto_ulong i = bodyStartIdx; i < an; ++i)
                letForm = letForm->appendLast(ctx, arityForm->getAt(ctx, static_cast<int>(i)));
            // The arity becomes ([params] (let [...] body...)).
            dscope.setAutomaticLocal(1, ctx->newList()
                ->appendLast(ctx, PROTO_NONE)->appendLast(ctx, letForm->asObject(ctx))->asObject(ctx));
            arityForm = dscope.getAutomaticLocal(1)->asList(ctx);
            bodyStartIdx = 1;
        }
    }
    proto::proto_ulong n = arityForm->getSize(ctx);
    proto::proto_ulong rawCount = params->getSize(ctx);

    // Session 7/8/13 — scan for `& <rest>`. The rest can be:
    //   - a symbol (`& rest`): variadic, collects leftover positionals as
    //     a list — the session-7 shape, unchanged.
    //   - a map (`& {:keys [k1 k2 ...]}`): kw-based, names each
    //     declared key as a local slot whose value will be supplied by
    //     the kwArgs map the VM builds from the caller's trailing
    //     key/value pairs at CALL_KW time (or the caller's trailing map).
    //
    // The two shapes are mutually exclusive — JVM-Clojure shares the
    // surface (`& {:keys}` is the kw form of variadic), but the
    // dispatch path diverges entirely. We pick the path by inspecting
    // the form after `&`.
    proto::proto_ulong fixedArity = rawCount;
    bool isVariadic = false;
    bool isKwBased  = false;
    std::string restName;
    std::string asBindName;  // `:as` snapshot binding name
    // Session 14 — each kw key may have an associated default form from
    // `:or {key default}`. defaultForm = nullptr when no default.
    struct KwKeyDecl {
        std::string name;
        const proto::ProtoObject* defaultForm;
    };
    std::vector<KwKeyDecl> kwKeyDecls;
    for (proto::proto_ulong i = 0; i < rawCount; ++i) {
        const proto::ProtoObject* p = params->getAt(ctx, (int)i);
        if (isNameForm(ctx, markers.named, p) && nameOf(ctx, markers.named, p) == "&") {
            if (i + 1 != rawCount - 1) {
                throw CompileError(
                    "fn/defn: `&` must be followed by exactly one form");
            }
            const proto::ProtoObject* rp = params->getAt(ctx, (int)(i + 1));
            fixedArity = i;

            if (isMap(rp)) {
                // `& {:keys [k1 k2] :or {k1 default} :as opts}` — kw
                // destructuring.
                isKwBased = true;
                // The map's entries, in its (unspecified) iteration order;
                // every entry is independent of the others.
                const std::vector<const proto::ProtoObject*> entries = mapFormEntries(ctx, rp);
                const std::size_t ne = entries.size();
                bool sawKeys = false;
                // Defer reading :or until after :keys — so we can attach
                // defaults to the right KwKeyDecl whatever the order.
                std::vector<const proto::ProtoObject*> orEntries;
                for (std::size_t j = 0; j < ne; j += 2) {
                    const proto::ProtoObject* mk = entries[j];
                    if (!isNameForm(ctx, markers.named, mk))
                        throw CompileError("fn/defn: `& {...}` key must be a keyword");
                    std::string mkName = nameOf(ctx, markers.named, mk);
                    const proto::ProtoObject* mv = entries[j + 1];
                    if (mkName == ":keys") {
                        if (!isVector(mv)) {
                            throw CompileError(
                                "fn/defn: `:keys` must be a vector of symbols");
                        }
                        const proto::ProtoList* ks = vectorItems(ctx, mv);
                        proto::proto_ulong nk = ks->getSize(ctx);
                        for (proto::proto_ulong t = 0; t < nk; ++t) {
                            const proto::ProtoObject* sym = ks->getAt(ctx, (int)t);
                            if (!isNameForm(ctx, markers.named, sym))
                                throw CompileError("fn/defn: `:keys` entry must be a symbol");
                            kwKeyDecls.push_back({nameOf(ctx, markers.named, sym), nullptr});
                        }
                        sawKeys = true;
                    } else if (mkName == ":or") {
                        if (!isMap(mv)) {
                            throw CompileError("fn/defn: `:or` must be a map");
                        }
                        orEntries = mapFormEntries(ctx, mv);
                    } else if (mkName == ":as") {
                        if (!isNameForm(ctx, markers.named, mv)) {
                            throw CompileError("fn/defn: `:as` binding must be a symbol");
                        }
                        asBindName = nameOf(ctx, markers.named, mv);
                    } else {
                        throw CompileError(
                            std::string("fn/defn: unsupported entry in `& {...}`: ") + mkName);
                    }
                }
                if (!sawKeys) {
                    throw CompileError("fn/defn: `& {...}` needs at least :keys");
                }
                // Attach :or defaults to the matching KwKeyDecl.
                if (!orEntries.empty()) {
                    const std::size_t nor = orEntries.size();
                    for (std::size_t j = 0; j < nor; j += 2) {
                        const proto::ProtoObject* ok = orEntries[j];
                        if (!isNameForm(ctx, markers.named, ok))
                            throw CompileError("fn/defn: `:or` key must be a symbol");
                        std::string okName = nameOf(ctx, markers.named, ok);
                        const proto::ProtoObject* ov = orEntries[j + 1];
                        bool matched = false;
                        for (auto& d : kwKeyDecls) {
                            if (d.name == okName) {
                                d.defaultForm = ov;
                                matched = true;
                                break;
                            }
                        }
                        if (!matched) {
                            throw CompileError(
                                "fn/defn: `:or` key not in :keys: " + okName);
                        }
                    }
                }
            } else if (isNameForm(ctx, markers.named, rp)) {
                // `& rest` — variadic, session-7 shape.
                restName   = nameOf(ctx, markers.named, rp);
                if (isQualifiedName(restName))
                    throw CompileError("Can't use qualified name as parameter: " + restName);
                isVariadic = true;
            } else {
                throw CompileError(
                    "fn/defn: form after `&` must be a symbol (variadic) or a `{:keys [...]}` map");
            }
            break;
        }
    }

    auto body = std::make_unique<BytecodeModule>();
    body->setArity(static_cast<int>(fixedArity));
    body->setVariadic(isVariadic);
    body->setKwBased(isKwBased);

    // Push a fresh fn scope. Fixed params go in slots 0..fixedArity-1; the
    // rest-binding (if any) gets slot fixedArity. We never hold a Scope&
    // across a recursive compileForm() — that call may compile a nested
    // fn, push_back another Scope, and reallocate `scopes_`, which would
    // dangle the reference. Always re-acquire via scopes_.back().
    scopes_.push_back(Scope{});
    {
        Scope& scope = scopes_.back();
        scope.arity = static_cast<int>(fixedArity);
        for (proto::proto_ulong i = 0; i < fixedArity; ++i) {
            const proto::ProtoObject* p = params->getAt(ctx, (int)i);
            if (!isNameForm(ctx, markers.named, p)) {
                scopes_.pop_back();
                throw CompileError("fn/defn: parameter name must be a symbol");
            }
            const std::string pname = nameOf(ctx, markers.named, p);
            if (isQualifiedName(pname)) {
                // A syntax-quoted parameter without `#` or `~'` comes out
                // qualified, `user/x`; Clojure rejects it the same way.
                scopes_.pop_back();
                throw CompileError("Can't use qualified name as parameter: " + pname);
            }
            scope.nameToSlot[pname] = static_cast<int>(i);
        }
        scope.nextSlot = static_cast<int>(fixedArity);
        if (isVariadic) {
            int restSlot = scope.nextSlot++;
            scope.nameToSlot[restName] = restSlot;
        }
        if (isKwBased) {
            for (const auto& d : kwKeyDecls) {
                int slot = scope.nextSlot++;
                scope.nameToSlot[d.name] = slot;
                body->addKwKey(d.name, slot,
                    internNamed(ctx, markers.named, (":" + d.name).c_str()));
            }
            if (!asBindName.empty()) {
                int slot = scope.nextSlot++;
                scope.nameToSlot[asBindName] = slot;
                body->setAsSlot(slot);
            }
        }
    }

    // Session 14 — `:or` default prologue. For each kw key with a
    // declared default, emit:
    //     PUSH_LOCAL <slot>
    //     PUSH_NIL
    //     EQ
    //     JUMP_IF_FALSE skip
    //     <compile default>
    //     STORE_LOCAL <slot>
    //     skip:
    // The slot was populated by the dispatcher (or left nil if the key
    // was missing). The `:or` default fires for either case; that
    // collapses the "missing" / "explicit nil" distinction, which is a
    // known v0.14 deviation from JVM-Clojure documented in STATUS.
    if (isKwBased) {
        for (std::size_t ki = 0; ki < kwKeyDecls.size(); ++ki) {
            if (!kwKeyDecls[ki].defaultForm) continue;
            int slot = body->kwKeys()[ki].localSlot;
            body->emit(Op::PUSH_LOCAL, slot);
            body->emit(Op::PUSH_NIL, 0);
            body->emit(Op::EQ, 0);
            std::size_t jifAt = body->emit(Op::JUMP_IF_FALSE, 0);
            compileForm(ctx, kwKeyDecls[ki].defaultForm, *body, markers);
            body->emit(Op::STORE_LOCAL, slot);
            std::size_t after = body->pos();
            std::size_t off = after - (jifAt + 1);
            body->patchOperand(jifAt, off);
        }
    }

    // Session 8 — implicit recur target at the top of the body. recur
    // rebinds the fn's fixed params and jumps back to the body's first
    // instruction. (A variadic param could be supported too, but Clojure
    // semantics for recur over &rest require subtler handling; defer.)
    {
        Scope::RecurTarget tgt;
        tgt.bodyStart = body->pos();
        for (int i = 0; i < static_cast<int>(fixedArity); ++i) tgt.slots.push_back(i);
        scopes_.back().recurStack.push_back(tgt);
    }

    // Compile the body forms after the params vector.
    if (bodyStartIdx >= n) {
        // Empty body → return nil.
        body->emit(Op::PUSH_NIL, 0);
    } else {
        for (proto::proto_ulong i = bodyStartIdx; i < n; ++i) {
            compileForm(ctx, arityForm->getAt(ctx, (int)i), *body, markers);
            if (i + 1 < n) body->emit(Op::POP, 0);
        }
    }
    body->emit(Op::RETURN, 0);
    if (!scopes_.back().recurStack.empty()) {
        scopes_.back().recurStack.pop_back();
    }

    // Re-acquire — nested fn compilation may have grown scopes_ and
    // invalidated any reference held before the body loop.
    Scope& scope = scopes_.back();
    body->setLocalCount(scope.nextSlot - scope.arity);
    for (const auto& c : scope.captures) {
        body->addCapture(c.parentSlot, c.localSlot);
    }
    scopes_.pop_back();
    return body;
}

void Compiler::compileTry(proto::ProtoContext* ctx, const proto::ProtoList* lst,
                          BytecodeModule& out, const CompilerMarkers& markers) {
    // A try needs local slots (the caught exception), and a top-level form
    // has none: compile a top-level try as the body of a zero-argument fn
    // called on the spot, `((fn [] (try ...)))`, as the `future` macro does for its
    // body.
    if (scopes_.empty()) {
        compileAsThunk(ctx, lst->asObject(ctx), out, markers);
        return;
    }

    // Split the form into body forms, catch clauses and the finally clause,
    // with JVM Clojure's rules and messages: body forms first, then catch
    // clauses, then at most one finally, last.
    // A clause naming a built-in class (classId >= 0) is matched by class
    // id; any other name is a type resolved at run time, when an exception
    // reaches the clause (classId < 0): a local, a var, or a module member
    // `alias/Type`.
    struct CatchClause {
        int classId;
        std::string className;
        std::string binding;
        const proto::ProtoList* form;   // (catch Class name body...)
    };
    const proto::proto_ulong n = lst->getSize(ctx);
    std::vector<proto::proto_ulong> bodyIdx;
    std::vector<CatchClause> catches;
    const proto::ProtoList* finallyForm = nullptr;
    for (proto::proto_ulong i = 1; i < n; ++i) {
        const proto::ProtoObject* f = lst->getAt(ctx, static_cast<int>(i));
        std::string clause;
        if (isList(f)) {
            const proto::ProtoList* fl = f->asList(ctx);
            if (fl->getSize(ctx) > 0 && isNameForm(ctx, markers.named, fl->getAt(ctx, 0))) {
                clause = nameOf(ctx, markers.named, fl->getAt(ctx, 0));
            }
        }
        if (finallyForm) {
            throw CompileError("finally clause must be last in try expression");
        }
        if (clause == "catch") {
            const proto::ProtoList* cl = f->asList(ctx);
            if (cl->getSize(ctx) < 3 || !isNameForm(ctx, markers.named, cl->getAt(ctx, 1)) ||
                !isNameForm(ctx, markers.named, cl->getAt(ctx, 2))) {
                throw CompileError(
                    "catch: expects (catch ClassName binding body...)");
            }
            const std::string className = nameOf(ctx, markers.named, cl->getAt(ctx, 1));
            // `Object` catches every value, as `Throwable` does.
            const int classId = (className == "Object" || className == "java.lang.Object")
                ? kThrowableClassId : exceptionClassId(className);
            catches.push_back({classId, className, nameOf(ctx, markers.named, cl->getAt(ctx, 2)), cl});
        } else if (clause == "finally") {
            finallyForm = f->asList(ctx);
        } else {
            if (!catches.empty()) {
                throw CompileError(
                    "Only catch or finally clause can follow catch in try expression");
            }
            bodyIdx.push_back(i);
        }
    }

    // Everything the try compiles — body, catch clauses and finally — is
    // inside it for `recur` (Scope::tryDepth). The scope is re-acquired by
    // index: compiling nested fns grows scopes_.
    const std::size_t scopeIdx = scopes_.size() - 1;
    struct DepthGuard {
        std::vector<Scope>& scopes; std::size_t idx;
        DepthGuard(std::vector<Scope>& s, std::size_t i) : scopes(s), idx(i) {
            ++scopes[idx].tryDepth;
        }
        ~DepthGuard() { if (idx < scopes.size()) --scopes[idx].tryDepth; }
    } depthGuard(scopes_, scopeIdx);

    auto patchTo = [&](std::size_t at, std::size_t target) {
        out.patchOperand(at, target - (at + 1));
    };
    // Forms [from, size) of `form` as an implicit do: nil when there are none.
    auto compileDo = [&](const proto::ProtoList* form, proto::proto_ulong from) {
        const proto::proto_ulong size = form->getSize(ctx);
        if (from >= size) { out.emit(Op::PUSH_NIL, 0); return; }
        for (proto::proto_ulong i = from; i < size; ++i) {
            compileForm(ctx, form->getAt(ctx, static_cast<int>(i)), out, markers);
            if (i + 1 < size) out.emit(Op::POP, 0);
        }
    };
    // The finally forms, for effect only: the try's value is left alone.
    auto compileFinally = [&]() {
        const proto::proto_ulong size = finallyForm->getSize(ctx);
        for (proto::proto_ulong i = 1; i < size; ++i) {
            compileForm(ctx, finallyForm->getAt(ctx, static_cast<int>(i)), out, markers);
            out.emit(Op::POP, 0);
        }
    };

    // A finally handler encloses the body AND the catch clauses, so it also
    // runs when a catch clause throws:
    //
    //       TRY_BEGIN Lfinally          ; only with a finally clause
    //       TRY_BEGIN Lcatch            ; only with catch clauses
    //       <body>
    //       TRY_END
    //       JUMP Ldone
    //   Lcatch:                         ; stack: exception
    //       STORE_CAUGHT ex             ; the slot, and the C++ exception
    //       PUSH_LOCAL ex, EXC_MATCH C1, JUMP_IF_FALSE L2, <catch body 1>, JUMP Ldone
    //   L2: ...
    //       RETHROW ex                  ; no clause matched: the original
    //   Ldone:
    //       TRY_END                     ; the finally handler
    //       <finally>, POP              ; normal path
    //       JUMP Lend
    //   Lfinally:                       ; stack: exception
    //       STORE_CAUGHT fx
    //       <finally>, POP
    //       RETHROW fx
    //   Lend:
    //
    // The finally forms are compiled twice, once per path, as javac does.
    std::size_t finallyBeginAt = 0;
    if (finallyForm) finallyBeginAt = out.emit(Op::TRY_BEGIN, 0);

    if (catches.empty()) {
        if (bodyIdx.empty()) out.emit(Op::PUSH_NIL, 0);
        for (std::size_t k = 0; k < bodyIdx.size(); ++k) {
            compileForm(ctx, lst->getAt(ctx, static_cast<int>(bodyIdx[k])), out, markers);
            if (k + 1 < bodyIdx.size()) out.emit(Op::POP, 0);
        }
    } else {
        const std::size_t catchBeginAt = out.emit(Op::TRY_BEGIN, 0);
        if (bodyIdx.empty()) out.emit(Op::PUSH_NIL, 0);
        for (std::size_t k = 0; k < bodyIdx.size(); ++k) {
            compileForm(ctx, lst->getAt(ctx, static_cast<int>(bodyIdx[k])), out, markers);
            if (k + 1 < bodyIdx.size()) out.emit(Op::POP, 0);
        }
        out.emit(Op::TRY_END, 0);
        std::vector<std::size_t> doneJumps{out.emit(Op::JUMP, 0)};

        patchTo(catchBeginAt, out.pos());
        const int exSlot = scopes_[scopeIdx].nextSlot++;
        out.emit(Op::STORE_CAUGHT, exSlot);
        for (const CatchClause& c : catches) {
            out.emit(Op::PUSH_LOCAL, exSlot);
            if (c.classId >= 0) {
                out.emit(Op::EXC_MATCH, static_cast<std::size_t>(c.classId));
            } else {
                compileCatchType(ctx, c.className, out);
                out.emit(Op::EXC_MATCH_TYPE, out.addSymbol(ctx, c.className));
            }
            const std::size_t nextAt = out.emit(Op::JUMP_IF_FALSE, 0);
            // The binding names the exception's slot for the clause body
            // only; an outer binding of the same name is restored after.
            int shadowed = -1;
            {
                auto& names = scopes_[scopeIdx].nameToSlot;
                auto it = names.find(c.binding);
                if (it != names.end()) shadowed = it->second;
                names[c.binding] = exSlot;
            }
            compileDo(c.form, 3);
            {
                auto& names = scopes_[scopeIdx].nameToSlot;
                if (shadowed >= 0) names[c.binding] = shadowed;
                else               names.erase(c.binding);
            }
            doneJumps.push_back(out.emit(Op::JUMP, 0));
            patchTo(nextAt, out.pos());
        }
        out.emit(Op::RETHROW, exSlot);
        for (std::size_t at : doneJumps) patchTo(at, out.pos());
    }

    if (finallyForm) {
        out.emit(Op::TRY_END, 0);
        compileFinally();
        const std::size_t endAt = out.emit(Op::JUMP, 0);
        patchTo(finallyBeginAt, out.pos());
        const int fxSlot = scopes_[scopeIdx].nextSlot++;
        out.emit(Op::STORE_CAUGHT, fxSlot);
        compileFinally();
        out.emit(Op::RETHROW, fxSlot);
        patchTo(endAt, out.pos());
    }
}

// Pushes the type a catch clause names when it is not a built-in class: a
// local, member `b` of a local `a` for `a/b`, or else a run-time lookup of
// the global (or the global's member) by RESOLVE_CLASS.
void Compiler::compileCatchType(proto::ProtoContext* ctx, const std::string& qualified,
                                BytecodeModule& out) {
    // `user/T`, from a syntax-quote, names the program's global T.
    const std::string name = isUserQualified(qualified) ? qualified.substr(5) : qualified;
    if (int slot = resolveLocal(name); slot >= 0) {
        out.emit(Op::PUSH_LOCAL, slot);
        return;
    }
    if (isQualifiedName(name)) {
        const std::size_t slash = name.find('/');
        if (int slot = resolveLocal(name.substr(0, slash)); slot >= 0) {
            out.emit(Op::PUSH_LOCAL, slot);
            out.emit(Op::GET_MEMBER, out.addSymbol(ctx, name.substr(slash + 1)));
            return;
        }
    }
    out.emit(Op::RESOLVE_CLASS, out.addSymbol(ctx, name));
}

// Pushes the receiver `a` of a qualified name `a/b`: a local, or a global
// looked up at run time.
void Compiler::compileQualifiedPrefix(proto::ProtoContext* ctx, const std::string& name,
                                      BytecodeModule& out) {
    const std::string prefix = name.substr(0, name.find('/'));
    if (int slot = resolveLocal(prefix); slot >= 0) {
        out.emit(Op::PUSH_LOCAL, slot);
    } else {
        out.emit(Op::PUSH_VAR, out.addSymbol(ctx, prefix));
    }
}

bool isQualifiedName(const std::string& name) {
    if (name.size() < 3 || name[0] == ':') return false;
    const std::size_t slash = name.find('/');
    return slash != std::string::npos && slash > 0 && slash + 1 < name.size();
}

int Compiler::resolveLocal(const std::string& name) {
    if (scopes_.empty()) return -1;

    // Find the deepest scope at index `foundIdx` that has `name`.
    int top = static_cast<int>(scopes_.size()) - 1;
    int foundIdx = -1;
    for (int i = top; i >= 0; --i) {
        if (scopes_[i].nameToSlot.count(name)) {
            foundIdx = i;
            break;
        }
    }
    if (foundIdx < 0)      return -1;
    if (foundIdx == top)   return scopes_[top].nameToSlot[name];

    // Found in an outer scope. Walk back upward, creating a capture in
    // every intermediate scope that does not already host `name`. Each
    // intermediate capture sources its value from the immediately-
    // enclosing scope's slot for `name` (which we just created the
    // previous iteration if it wasn't already there).
    int sourceSlot = scopes_[foundIdx].nameToSlot[name];
    for (int i = foundIdx + 1; i <= top; ++i) {
        auto& s = scopes_[i];
        auto it = s.nameToSlot.find(name);
        if (it != s.nameToSlot.end()) {
            // Already a slot in this scope (a previous capture or a
            // genuine local) — adopt it as the source for the next level.
            sourceSlot = it->second;
            continue;
        }
        int newSlot = s.nextSlot++;
        s.nameToSlot[name] = newSlot;
        s.captures.push_back({sourceSlot, newSlot});
        sourceSlot = newSlot;
    }
    return scopes_[top].nameToSlot[name];
}

bool Compiler::isLocalName(const std::string& name) const {
    for (const Scope& s : scopes_) {
        if (s.nameToSlot.count(name)) return true;
    }
    return false;
}

const proto::ProtoObject* Compiler::macroForHead(proto::ProtoContext* ctx,
                                                 const proto::ProtoObject* head,
                                                 const CompilerMarkers& markers,
                                                 std::string& name) const {
    const ActiveCallContext* cc = activeCallContext();
    if (!cc || !cc->engine || !isNameForm(ctx, markers.named, head)) return nullptr;
    name = nameOf(ctx, markers.named, head);
    std::string key;
    if (!globalKeyForSymbol(name, key)) return nullptr;  // a keyword
    std::string bare = key;
    const bool qualified = stripCoreQualifier(bare) || key != name;
    if (isSpecialFormName(bare)) return nullptr;
    if (!qualified && isLocalName(key)) return nullptr;  // a local shadows a macro
    return globalMacro(ctx, *cc, key);
}

const proto::ProtoObject* Compiler::localEnv(proto::ProtoContext* ctx,
                                             const CompilerMarkers& markers) const {
    if (scopes_.empty()) return PROTO_NONE;
    std::vector<const proto::ProtoObject*> kv;
    std::unordered_map<std::string, bool> seen;
    for (const Scope& s : scopes_) {
        for (const auto& entry : s.nameToSlot) {
            if (!seen.emplace(entry.first, true).second) continue;
            // Interned symbols are never collected: the vector needs no root.
            const proto::ProtoObject* sym = internNamed(ctx, markers.named, entry.first.c_str());
            kv.push_back(sym);
            kv.push_back(sym);
        }
    }
    if (kv.empty()) return PROTO_NONE;
    return mapAssocPairs(ctx, nullptr, kv.data(), kv.size());
}

void Compiler::compileMacroCall(proto::ProtoContext* ctx, const proto::ProtoObject* form,
                                const proto::ProtoObject* macro, const std::string& name,
                                BytecodeModule& out, const CompilerMarkers& markers) {
    // Slot 0 roots the macro (a later `def` may replace the global while it
    // runs), slot 1 the environment, slot 2 the expansion while it is
    // compiled. The call form is rooted by the caller.
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(3);
    scope.setAutomaticLocal(0, macro);
    scope.setAutomaticLocal(1, localEnv(&scope, markers));
    const ActiveCallContext* cc = activeCallContext();
    try {
        scope.setAutomaticLocal(2, expandMacroCall(&scope, *cc, scope.getAutomaticLocal(0), name,
                                                   form, scope.getAutomaticLocal(1)));
    } catch (const proto::ProtoException& e) {
        throw CompileError("Syntax error macroexpanding " + name + ": " +
                           describeThrown(&scope, e.value()));
    }
    compileForm(&scope, scope.getAutomaticLocal(2), out, markers);
}

const proto::ProtoObject* Compiler::macroexpandTopLevel(proto::ProtoContext* ctx,
                                                        const proto::ProtoObject* form,
                                                        const CompilerMarkers& markers) {
    // Recursive, as Clojure's Compiler.macroexpand is: a macro that always
    // expands into another call of itself ends in StackOverflowError.
    checkNativeStack(StackUse::Source);
    if (!isList(form)) return form;
    const proto::ProtoList* lst = form->asList(ctx);
    if (lst->getSize(ctx) == 0) return form;
    std::string name;
    const proto::ProtoObject* macro = macroForHead(ctx, lst->getAt(ctx, 0), markers, name);
    if (!macro) return form;
    // The expansion is anchored in `ctx` (expandMacroCall), and so is each
    // intermediate one: they live until the caller's context ends.
    const proto::ProtoObject* expansion = nullptr;
    try {
        expansion = expandMacroCall(ctx, *activeCallContext(), macro, name, form, PROTO_NONE);
    } catch (const proto::ProtoException& e) {
        throw CompileError("Syntax error macroexpanding " + name + ": " +
                           describeThrown(ctx, e.value()));
    }
    return macroexpandTopLevel(ctx, expansion, markers);
}

void Compiler::compileAsThunk(proto::ProtoContext* ctx, const proto::ProtoObject* form,
                              BytecodeModule& out, const CompilerMarkers& markers) {
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(1);
    scope.setAutomaticLocal(0, scope.newList()
        ->appendLast(&scope, symbolForm(&scope, markers.named, "fn"))
        ->appendLast(&scope, form)
        ->asObject(&scope));
    std::unique_ptr<BytecodeModule> body =
        compileArity(&scope, scope.newList(), scope.getAutomaticLocal(0)->asList(&scope),
                     /*bodyStartIdx=*/1, markers);
    std::size_t blockIdx = out.addBlock(std::move(body));
    out.emit(Op::MAKE_FN, blockIdx);   // no captures at the top level
    out.emit(Op::CALL, 0);
}

void Compiler::compileDefmacro(proto::ProtoContext* ctx, const proto::ProtoList* lst,
                               BytecodeModule& out, const CompilerMarkers& markers) {
    const proto::proto_ulong n = lst->getSize(ctx);
    if (n < 3) throw CompileError("defmacro: expects (defmacro name [params] body...)");
    const proto::ProtoObject* nameForm = lst->getAt(ctx, 1);
    std::string key;
    if (!isNameForm(ctx, markers.named, nameForm) ||
        !globalKeyForSymbol(nameOf(ctx, markers.named, nameForm), key)) {
        throw CompileError("defmacro: name must be a symbol");
    }
    // An optional docstring and attribute map come before the arities.
    proto::proto_ulong i = 2;
    if (i < n && isStringForm(lst->getAt(ctx, static_cast<int>(i))) && i + 1 < n) ++i;
    if (i < n && isMap(lst->getAt(ctx, static_cast<int>(i)))) ++i;
    if (i >= n) throw CompileError("defmacro: missing parameter vector");

    // Every arity gains the hidden parameters &form and &env in front.
    // Slot 0 roots the fn form being built, slot 1 the arity being built.
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(2);
    const proto::ProtoObject* amp_form = symbolForm(&scope, markers.named, "&form");
    const proto::ProtoObject* amp_env  = symbolForm(&scope, markers.named, "&env");
    auto withHidden = [&](const proto::ProtoObject* params) -> const proto::ProtoObject* {
        const proto::ProtoList* items = nullptr;
        if (isVector(params)) items = vectorItems(&scope, params);
        else throw CompileError("defmacro: parameter list must be a vector");
        const proto::ProtoList* full = scope.newList()
            ->appendLast(&scope, amp_form)->appendLast(&scope, amp_env);
        const proto::proto_ulong pn = items->getSize(&scope);
        for (proto::proto_ulong k = 0; k < pn; ++k)
            full = full->appendLast(&scope, items->getAt(&scope, static_cast<int>(k)));
        return newVector(&scope, full);
    };
    scope.setAutomaticLocal(0, scope.newList()
        ->appendLast(&scope, symbolForm(&scope, markers.named, "fn"))->asObject(&scope));
    const proto::ProtoObject* first = lst->getAt(ctx, static_cast<int>(i));
    if (isVector(first)) {
        const proto::ProtoList* fnForm = scope.getAutomaticLocal(0)->asList(&scope)
            ->appendLast(&scope, withHidden(first));
        for (proto::proto_ulong k = i + 1; k < n; ++k)
            fnForm = fnForm->appendLast(&scope, lst->getAt(&scope, static_cast<int>(k)));
        scope.setAutomaticLocal(0, fnForm->asObject(&scope));
    } else {
        for (proto::proto_ulong k = i; k < n; ++k) {
            const proto::ProtoObject* arity = lst->getAt(&scope, static_cast<int>(k));
            if (!isList(arity) || arity->asList(&scope)->getSize(&scope) == 0) {
                if (k + 1 == n && isMap(arity)) break;  // a trailing attribute map
                throw CompileError("defmacro: each arity must be a list ([params] body...)");
            }
            const proto::ProtoList* al = arity->asList(&scope);
            const proto::ProtoList* rebuilt = scope.newList()
                ->appendLast(&scope, withHidden(al->getAt(&scope, 0)));
            const proto::proto_ulong an = al->getSize(&scope);
            for (proto::proto_ulong t = 1; t < an; ++t)
                rebuilt = rebuilt->appendLast(&scope, al->getAt(&scope, static_cast<int>(t)));
            scope.setAutomaticLocal(1, rebuilt->asObject(&scope));
            scope.setAutomaticLocal(0, scope.getAutomaticLocal(0)->asList(&scope)
                ->appendLast(&scope, scope.getAutomaticLocal(1))->asObject(&scope));
        }
    }
    // (def name (__macro__ (fn ...))): __macro__ returns the fn wrapper with
    // the macro flag set (Macros.h).
    out.emit(Op::PUSH_VAR, out.addSymbol(&scope, "__macro__"));
    compileForm(&scope, scope.getAutomaticLocal(0), out, markers);
    out.emit(Op::CALL, 1);
    emitStoreGlobal(&scope, key, out);
}

void Compiler::compileForm(proto::ProtoContext* ctx,
                           const proto::ProtoObject* form,
                           BytecodeModule& out,
                           const CompilerMarkers& markers) {
    // Every nested form recurses through here: forms nested deeper than the
    // thread's stack allows raise StackOverflowError, which the drivers
    // report as a compile error.
    checkNativeStack(StackUse::Source);
    if (form == nullptr) {
        throw CompileError("compile: internal error: null form");
    }
    // nil, true and false: the reader yields the values themselves.
    if (form == PROTO_NONE)  { out.emit(Op::PUSH_NIL,   0); return; }
    if (form == PROTO_TRUE)  { out.emit(Op::PUSH_TRUE,  0); return; }
    if (form == PROTO_FALSE) { out.emit(Op::PUSH_FALSE, 0); return; }

    // Integer literal — emit PUSH_CONST <addLong>.
    if (form->isInteger(ctx)) {
        std::size_t idx = addIntegerConst(ctx, form, out);
        out.emit(Op::PUSH_CONST, idx);
        return;
    }

    // Float literal — emit PUSH_CONST <addDouble>.
    if (form->isFloat(ctx)) {
        std::size_t idx = out.addDouble(form->asDouble(ctx));
        out.emit(Op::PUSH_CONST, idx);
        return;
    }

    // List form: either a special form or a call form. We decide by the
    // head symbol's text: a fixed set of names dispatch as special forms,
    // anything else is a plain call.
    if (isList(form)) {
        const proto::ProtoList* lst = form->asList(ctx);
        proto::proto_ulong n = lst->getSize(ctx);
        if (n == 0) {
            // `()` evaluates to the empty list, as in Clojure.
            out.emit(Op::PUSH_CONST, out.addDatum(ctx, form));
            return;
        }
        const proto::ProtoObject* head = lst->getAt(ctx, 0);
        // Special-form dispatch keys on the head's text; only symbols
        // qualify. A non-stringy head (e.g. `((outer 100) 20)` where the
        // callable is itself an expression) is always a regular call and
        // is compiled below via compileForm on the head.
        std::string headName = isNameForm(ctx, markers.named, head) ? nameOf(ctx, markers.named, head) : std::string{};
        // `clojure.core/x` names the built-in x (what syntax-quote and the
        // reader macros produce): dispatched as x, never shadowed by a local.
        const bool coreQualified = stripCoreQualifier(headName);

        // ---- Macro calls ---------------------------------------------------
        // A head naming a macro (Macros.h) is expanded here, at compile time,
        // and the expansion compiled in the call's place. The macro table is
        // the globals as they stand now: a macro defined by a form that has
        // already run is in effect, a later redefinition is not.
        {
            std::string macroName;
            if (const proto::ProtoObject* macro = macroForHead(ctx, head, markers, macroName)) {
                compileMacroCall(ctx, form, macro, macroName, out, markers);
                return;
            }
        }

        if (headName == "defmacro") {
            compileDefmacro(ctx, lst, out, markers);
            return;
        }

        // ---- Special forms -------------------------------------------------

        // (def name expr) — compile expr, store in globals under `name`.
        // The store leaves the value on the stack as `def`'s result, matching
        // Clojure semantics (def returns the Var; we return the value for
        // simplicity — close enough for the conformance tests).
        if (headName == "def") {
            if (n != 3) throw CompileError("def: expects (def name expr)");
            const proto::ProtoObject* nameForm = lst->getAt(ctx, 1);
            const proto::ProtoObject* exprForm = lst->getAt(ctx, 2);
            std::string key;
            if (!isNameForm(ctx, markers.named, nameForm) ||
                !globalKeyForSymbol(nameOf(ctx, markers.named, nameForm), key)) {
                throw CompileError("def: name must be a symbol");
            }
            compileForm(ctx, exprForm, out, markers);
            emitStoreGlobal(ctx, key, out);
            return;
        }

        // (if test then else?) — JUMP_IF_FALSE over then, JUMP over else.
        // Offsets are back-patched once the branch sizes are known.
        if (headName == "if") {
            if (n != 3 && n != 4) {
                throw CompileError("if: expects (if test then) or (if test then else)");
            }
            // A literal test decides the branch at compile time: `cond`'s
            // `:else` clause expands to (if :else expr ...), which then
            // costs nothing at run time, as the C++ cond's did.
            const proto::ProtoObject* test = lst->getAt(ctx, 1);
            const bool literalTrue = test == PROTO_TRUE || isKeywordForm(ctx, markers.named, test) ||
                                     isStringForm(test) || test->isInteger(ctx) || test->isFloat(ctx);
            const bool literalFalse = test == PROTO_NONE || test == PROTO_FALSE;
            if (literalTrue) { compileForm(ctx, lst->getAt(ctx, 2), out, markers); return; }
            if (literalFalse) {
                if (n == 4) compileForm(ctx, lst->getAt(ctx, 3), out, markers);
                else out.emit(Op::PUSH_NIL, 0);
                return;
            }
            compileForm(ctx, test, out, markers);  // test
            std::size_t jifAt = out.emit(Op::JUMP_IF_FALSE, 0);

            compileForm(ctx, lst->getAt(ctx, 2), out, markers);  // then
            std::size_t jmpAt = out.emit(Op::JUMP, 0);

            // patch the JIF to point at the start of the else branch
            std::size_t elseStart = out.pos();
            std::size_t elseOffsetInstr =
                elseStart - (jifAt + 1);
            out.patchOperand(jifAt,
                elseOffsetInstr);

            if (n == 4) {
                compileForm(ctx, lst->getAt(ctx, 3), out, markers);  // else
            } else {
                out.emit(Op::PUSH_NIL, 0);                  // implicit nil
            }

            // patch the JMP to point past the else branch
            std::size_t afterElse = out.pos();
            std::size_t pastOffsetInstr =
                afterElse - (jmpAt + 1);
            out.patchOperand(jmpAt,
                pastOffsetInstr);
            return;
        }

        // (and a b c ...) — short-circuit. Each value is evaluated; the
        // result is the first falsey, or the last truthy. Implementation:
        //   compile a, DUP, JUMP_IF_FALSE end, POP, compile b, DUP, ...
        if (headName == "and") {
            if (n == 1) { out.emit(Op::PUSH_TRUE, 0); return; }
            std::vector<std::size_t> shortAts;
            for (proto::proto_ulong i = 1; i < n; ++i) {
                compileForm(ctx, lst->getAt(ctx, (int)i), out, markers);
                if (i + 1 < n) {
                    out.emit(Op::DUP, 0);
                    shortAts.push_back(out.emit(Op::JUMP_IF_FALSE, 0));
                    out.emit(Op::POP, 0);  // drop the truthy copy left by DUP
                }
            }
            std::size_t end = out.pos();
            for (std::size_t at : shortAts) {
                std::size_t off = end - (at + 1);
                out.patchOperand(at, off);
            }
            return;
        }

        // (or a b c ...) — short-circuit. Result is the first truthy, or
        // the last value. Mirror of and using JUMP_IF_TRUE.
        if (headName == "or") {
            if (n == 1) { out.emit(Op::PUSH_NIL, 0); return; }
            std::vector<std::size_t> shortAts;
            for (proto::proto_ulong i = 1; i < n; ++i) {
                compileForm(ctx, lst->getAt(ctx, (int)i), out, markers);
                if (i + 1 < n) {
                    out.emit(Op::DUP, 0);
                    shortAts.push_back(out.emit(Op::JUMP_IF_TRUE, 0));
                    out.emit(Op::POP, 0);
                }
            }
            std::size_t end = out.pos();
            for (std::size_t at : shortAts) {
                std::size_t off = end - (at + 1);
                out.patchOperand(at, off);
            }
            return;
        }

        // (do form1 form2 ... formN) — evaluate each form, POP between for
        // statement-level discard. The last form's value is the do's value.
        if (headName == "do") {
            if (n == 1) {
                out.emit(Op::PUSH_NIL, 0);     // (do) => nil
                return;
            }
            for (proto::proto_ulong i = 1; i < n; ++i) {
                compileForm(ctx, lst->getAt(ctx, static_cast<int>(i)), out, markers);
                if (i + 1 < n) out.emit(Op::POP, 0);
            }
            return;
        }

        // (quote form) — push the form as a literal. For v0.0.x we only
        // support quoting atoms (numbers, strings, symbols). Quoting a list
        // requires materialising the data structure at runtime, which is a
        // later-session feature.
        // (quote form) — the form itself, unevaluated, as the value. A
        // symbol or keyword is its interned value (Named.h); a string,
        // number, nil or boolean the literal; a list, vector or map the very
        // value the reader yielded (or the macro built), pinned by the
        // module as a Datum constant (BytecodeModule.h), so every
        // evaluation answers the same immutable object, as in Clojure.
        if (headName == "quote") {
            if (n != 2) throw CompileError("quote: expects (quote form)");
            const proto::ProtoObject* q = lst->getAt(ctx, 1);
            if (isNameForm(ctx, markers.named, q)) {
                std::size_t idx = out.addNamed(nameOf(ctx, markers.named, q), q);
                out.emit(Op::PUSH_CONST, idx);
                return;
            }
            if (q == PROTO_NONE || q == PROTO_TRUE || q == PROTO_FALSE ||
                isStringForm(q) || q->isInteger(ctx) || q->isFloat(ctx)) {
                compileForm(ctx, q, out, markers);  // the literal itself
                return;
            }
            out.emit(Op::PUSH_CONST, out.addDatum(ctx, q));
            return;
        }

        // `~x` and `~@x` outside a syntax-quote: the reader yields
        // (clojure.core/unquote x), which only a syntax-quote consumes.
        if (coreQualified && (headName == "unquote" || headName == "unquote-splicing")) {
            throw CompileError(headName == "unquote"
                ? "unquote (~) used outside a syntax-quote (`)"
                : "unquote-splicing (~@) used outside a syntax-quote (`)");
        }

        // (fn ...) / (defn name ...) — single OR multi-arity. Multi-arity
        // is detected by inspecting the form immediately after the head
        // (and the name, for defn): if it is a list whose first element
        // is itself a list, the form is multi-arity. Each subsequent form
        // is then a `(params body...)` arity body.
        if (headName == "fn" || headName == "defn") {
            // (defn name "doc"? {attrs}? ...): the docstring and the attribute
            // map are dropped, there being no vars to hold them (D28).
            if (headName == "defn" && n > 3) {
                proto::proto_ulong skip = 2;
                if (isStringForm(lst->getAt(ctx, 2))) ++skip;
                if (skip < n - 1 && isMap(lst->getAt(ctx, static_cast<int>(skip)))) ++skip;
                if (skip > 2) {
                    proto::ProtoContext scope(ctx->space, ctx);
                    scope.resizeAutomaticLocals(1);
                    const proto::ProtoList* rebuilt = scope.newList()
                        ->appendLast(&scope, head)->appendLast(&scope, lst->getAt(&scope, 1));
                    for (proto::proto_ulong k = skip; k < n; ++k)
                        rebuilt = rebuilt->appendLast(&scope, lst->getAt(&scope, static_cast<int>(k)));
                    scope.setAutomaticLocal(0, rebuilt->asObject(&scope));
                    compileForm(&scope, scope.getAutomaticLocal(0), out, markers);
                    return;
                }
            }
            proto::proto_ulong aritiesStart = (headName == "fn") ? 1 : 2;
            if (n <= aritiesStart) {
                throw CompileError("fn/defn: missing parameter vector");
            }
            const proto::ProtoObject* nameForm = nullptr;
            if (headName == "defn") {
                nameForm = lst->getAt(ctx, 1);
                if (!isNameForm(ctx, markers.named, nameForm)) {
                    throw CompileError("defn: name must be a symbol");
                }
            }

            const proto::ProtoObject* first = lst->getAt(ctx, (int)aritiesStart);
            // Single-arity: the form right after the head/name is a
            // vector `[..]`. Multi-arity: a list whose first
            // element is itself a vector — `(fn ([x] ...) ([x y] ...))`.
            bool multiArity = false;
            if (isList(first)) {
                const proto::ProtoList* fl = first->asList(ctx);
                if (fl->getSize(ctx) >= 1 &&
                    isVector(fl->getAt(ctx, 0))) {
                    multiArity = true;
                }
            }

            auto emitCapsThenOp = [&](std::size_t blockIdx, Op makeOp,
                                      std::size_t makeOperand) {
                const auto& caps = out.block(blockIdx).captureSpecs();
                for (const auto& c : caps) {
                    out.emit(Op::PUSH_LOCAL,
                        c.parentSlot);
                }
                out.emit(makeOp, makeOperand);
            };

            if (!multiArity) {
                // Single-arity — the original single-body path.
                std::unique_ptr<BytecodeModule> body =
                    compileFnBody(ctx, lst, markers);
                std::size_t blockIdx = out.addBlock(std::move(body));
                emitCapsThenOp(blockIdx, Op::MAKE_FN,
                               blockIdx);
            } else {
                // Multi-arity — compile each `(params body...)` separately,
                // push their captures (arity 0 first), emit MAKE_FN_MULTI.
                std::vector<std::size_t> blockIdxs;
                for (proto::proto_ulong i = aritiesStart; i < n; ++i) {
                    const proto::ProtoObject* aobj = lst->getAt(ctx, (int)i);
                    if (!isList(aobj)) {
                        throw CompileError("multi-arity: each arity must be a list");
                    }
                    const proto::ProtoList* alist = aobj->asList(ctx);
                    if (alist->getSize(ctx) < 1) {
                        throw CompileError("multi-arity: arity missing params");
                    }
                    const proto::ProtoObject* paramsForm = alist->getAt(ctx, 0);
                    const proto::ProtoList* params = nullptr;
                    if (isVector(paramsForm)) {
                        params = vectorItems(ctx, paramsForm);
                    } else if (isList(paramsForm)) {
                        params = paramsForm->asList(ctx);
                    } else {
                        throw CompileError("multi-arity: params must be a vector");
                    }
                    std::unique_ptr<BytecodeModule> body =
                        compileArity(ctx, params, alist, 1, markers);
                    std::size_t blockIdx = out.addBlock(std::move(body));
                    blockIdxs.push_back(blockIdx);
                }
                // Push captures in arity-emission order so the VM can pop
                // them back into per-arity lists.
                for (std::size_t bi : blockIdxs) {
                    const auto& caps = out.block(bi).captureSpecs();
                    for (const auto& c : caps) {
                        out.emit(Op::PUSH_LOCAL,
                            c.parentSlot);
                    }
                }
                std::size_t groupIdx = out.addArityGroup(std::move(blockIdxs));
                out.emit(Op::MAKE_FN_MULTI,
                         groupIdx);
            }

            if (headName == "defn") {
                std::string key;
                globalKeyForSymbol(nameOf(ctx, markers.named, nameForm), key);
                emitStoreGlobal(ctx, key, out);
            }
            return;
        }

        // (let [name1 expr1 name2 expr2 ...] body...) — sequentially bind
        // each name to its expr in a freshly-allocated local slot, then
        // compile the body. Bindings are visible to subsequent binding RHSes
        // (Clojure's let* semantics).
        if (headName == "let") {
            // The top level has no local slots: a top-level let runs as the
            // body of a fn called on the spot.
            if (scopes_.empty()) { compileAsThunk(ctx, form, out, markers); return; }
            if (n < 2) throw CompileError("let: expects bindings vector");
            const proto::ProtoObject* bindings = lst->getAt(ctx, 1);
            const proto::ProtoList* bvec = nullptr;
            if (isVector(bindings)) {
                bvec = vectorItems(ctx, bindings);
            } else if (isList(bindings)) {
                bvec = bindings->asList(ctx);
            } else {
                throw CompileError("let: bindings must be a vector");
            }
            proto::proto_ulong bn = bvec->getSize(ctx);
            if (bn % 2 != 0) {
                throw CompileError("let: bindings must come in name/value pairs");
            }
            // Destructuring: `(let [[a b] v] ...)` is compiled as the `let`
            // of the symbol bindings it stands for (destructure), rooted in
            // a slot of its own scope while it is compiled.
            {
                proto::ProtoContext scope(ctx->space, ctx);
                scope.resizeAutomaticLocals(1);
                if (const proto::ProtoObject* flat = destructureBindings(&scope, markers, bvec)) {
                    scope.setAutomaticLocal(0, flat);
                    const proto::ProtoList* rebuilt = scope.newList()
                        ->appendLast(&scope, head)->appendLast(&scope, scope.getAutomaticLocal(0));
                    for (proto::proto_ulong i = 2; i < n; ++i)
                        rebuilt = rebuilt->appendLast(&scope, lst->getAt(&scope, static_cast<int>(i)));
                    scope.setAutomaticLocal(0, rebuilt->asObject(&scope));
                    compileForm(&scope, scope.getAutomaticLocal(0), out, markers);
                    return;
                }
            }
            // The names the bindings shadow, restored once the body is
            // compiled: a binding is in scope in the let's body only.
            BindingRestore restore(scopes_);
            for (proto::proto_ulong i = 0; i < bn; i += 2) {
                const proto::ProtoObject* nameForm = bvec->getAt(ctx, (int)i);
                const proto::ProtoObject* valForm  = bvec->getAt(ctx, (int)(i + 1));
                if (!isNameForm(ctx, markers.named, nameForm)) {
                    throw CompileError("let: binding name must be a symbol");
                }
                const std::string bname = nameOf(ctx, markers.named, nameForm);
                if (isQualifiedName(bname)) throw CompileError("Can't let qualified name: " + bname);
                compileForm(ctx, valForm, out, markers);  // push value
                int slot = restore.bind(bname);
                out.emit(Op::STORE_LOCAL, slot);
            }
            // Body — last expression's value stays on the stack as `let`'s
            // value.
            if (n == 2) {
                out.emit(Op::PUSH_NIL, 0);                  // (let [...]) => nil
            } else {
                for (proto::proto_ulong i = 2; i < n; ++i) {
                    compileForm(ctx, lst->getAt(ctx, (int)i), out, markers);
                    if (i + 1 < n) out.emit(Op::POP, 0);
                }
            }
            return;
        }

        // (loop [name1 expr1 ...] body...) — like let, plus a recur target
        // at the start of the body. recur rebinds the loop locals and jumps
        // back.
        if (headName == "loop") {
            if (scopes_.empty()) { compileAsThunk(ctx, form, out, markers); return; }
            if (n < 2) throw CompileError("loop: expects bindings vector");
            const proto::ProtoObject* bindings = lst->getAt(ctx, 1);
            const proto::ProtoList* bvec = nullptr;
            if (isVector(bindings)) {
                bvec = vectorItems(ctx, bindings);
            } else if (isList(bindings)) {
                bvec = bindings->asList(ctx);
            } else {
                throw CompileError("loop: bindings must be a vector");
            }
            proto::proto_ulong bn = bvec->getSize(ctx);
            if (bn % 2 != 0) {
                throw CompileError("loop: bindings must come in name/value pairs");
            }
            // Destructuring, as clojure.core/loop does it: a binding form that
            // is not a symbol is bound to a generated local g, which the loop
            // rebinds and the body destructures:
            //   (let [g v form g ...] (loop [g g ...] (let [form g ...] body)))
            // so later inits see the earlier destructured names.
            {
                bool simple = true;
                for (proto::proto_ulong i = 0; i < bn; i += 2) {
                    const proto::ProtoObject* b = bvec->getAt(ctx, static_cast<int>(i));
                    if (!isNameForm(ctx, markers.named, b) || isKeywordForm(ctx, markers.named, b)) simple = false;
                }
                if (!simple) {
                    proto::ProtoContext scope(ctx->space, ctx);
                    scope.resizeAutomaticLocals(1);
                    const proto::ProtoList* outer = scope.newList();
                    const proto::ProtoList* loopBs = scope.newList();
                    const proto::ProtoList* inner = scope.newList();
                    for (proto::proto_ulong i = 0; i < bn; i += 2) {
                        const proto::ProtoObject* b = bvec->getAt(&scope, static_cast<int>(i));
                        const proto::ProtoObject* v = bvec->getAt(&scope, static_cast<int>(i + 1));
                        const bool isSym = isNameForm(&scope, markers.named, b);
                        const proto::ProtoObject* g = isSym ? b : freshSymbol(&scope, markers.named, "p__");
                        outer = outer->appendLast(&scope, g)->appendLast(&scope, v);
                        if (!isSym) {
                            outer = outer->appendLast(&scope, b)->appendLast(&scope, g);
                            inner = inner->appendLast(&scope, b)->appendLast(&scope, g);
                        }
                        loopBs = loopBs->appendLast(&scope, g)->appendLast(&scope, g);
                    }
                    const proto::ProtoList* innerLet = scope.newList()
                        ->appendLast(&scope, symbolForm(&scope, markers.named, "let"))
                        ->appendLast(&scope, newVector(&scope, inner));
                    for (proto::proto_ulong i = 2; i < n; ++i)
                        innerLet = innerLet->appendLast(&scope, lst->getAt(&scope, static_cast<int>(i)));
                    const proto::ProtoObject* loopForm = listForm(&scope,
                        {head, newVector(&scope, loopBs), innerLet->asObject(&scope)});
                    scope.setAutomaticLocal(0, listForm(&scope,
                        {symbolForm(&scope, markers.named, "let"), newVector(&scope, outer), loopForm}));
                    compileForm(&scope, scope.getAutomaticLocal(0), out, markers);
                    return;
                }
            }
            std::vector<int> recurSlots;
            BindingRestore restore(scopes_);
            for (proto::proto_ulong i = 0; i < bn; i += 2) {
                const proto::ProtoObject* nameForm = bvec->getAt(ctx, (int)i);
                const proto::ProtoObject* valForm  = bvec->getAt(ctx, (int)(i + 1));
                if (!isNameForm(ctx, markers.named, nameForm)) {
                    throw CompileError("loop: binding name must be a symbol");
                }
                const std::string bname = nameOf(ctx, markers.named, nameForm);
                if (isQualifiedName(bname)) throw CompileError("Can't let qualified name: " + bname);
                compileForm(ctx, valForm, out, markers);
                int slot = restore.bind(bname);
                out.emit(Op::STORE_LOCAL, slot);
                recurSlots.push_back(slot);
            }
            // Recur target = current bytecode position. recur jumps BACK here
            // after rebinding.
            Scope::RecurTarget tgt;
            tgt.bodyStart = out.pos();
            tgt.slots = recurSlots;
            tgt.tryDepth = scopes_.back().tryDepth;
            scopes_.back().recurStack.push_back(tgt);

            if (n == 2) {
                out.emit(Op::PUSH_NIL, 0);
            } else {
                for (proto::proto_ulong i = 2; i < n; ++i) {
                    compileForm(ctx, lst->getAt(ctx, (int)i), out, markers);
                    if (i + 1 < n) out.emit(Op::POP, 0);
                }
            }

            scopes_.back().recurStack.pop_back();
            return;
        }

        // (try body* (catch Class name body*)* (finally body*)?)
        if (headName == "try") {
            compileTry(ctx, lst, out, markers);
            return;
        }

        // (throw expr) — raise the value of expr, which must be an exception.
        if (headName == "throw") {
            if (n != 2) {
                throw CompileError(
                    "Too many arguments to throw, throw expects a single exception");
            }
            compileForm(ctx, lst->getAt(ctx, 1), out, markers);
            out.emit(Op::THROW, 0);
            return;
        }

        // (apply f args-list) — special form. Compile f, compile the
        // args-list (any expression that yields a list), emit CALL_APPLY.
        // v0.7.x supports only the two-arg shape; the JVM-Clojure variadic
        // form `(apply f x y coll)` lands once multi-arity arrives.
        if (headName == "apply") {
            if (n != 3) {
                throw CompileError(
                    "apply: only the two-argument form (apply f coll) is supported");
            }
            compileForm(ctx, lst->getAt(ctx, 1), out, markers);
            compileForm(ctx, lst->getAt(ctx, 2), out, markers);
            out.emit(Op::CALL_APPLY, 0);
            return;
        }

        // (recur arg1 arg2 ... argN) — rebind the enclosing loop's locals
        // and jump back to its body start. arity must match.
        if (headName == "recur") {
            if (scopes_.empty() || scopes_.back().recurStack.empty()) {
                throw CompileError("recur: no enclosing loop in current scope");
            }
            // A copy, not a reference: compiling an argument that holds a fn
            // (`future`, `fn`) pushes a nested Scope and may reallocate
            // `scopes_`, which would leave a reference dangling.
            const auto tgt = scopes_.back().recurStack.back();
            if (tgt.tryDepth != scopes_.back().tryDepth) {
                throw CompileError("Cannot recur across try");
            }
            proto::proto_ulong argc = n - 1;
            if (argc != tgt.slots.size()) {
                throw CompileError("recur: arity mismatch with enclosing loop");
            }
            // Compile all args left-to-right; the last one ends on top of
            // the stack. Then store each into its slot in REVERSE order so
            // the rightmost (top-of-stack) value goes into the rightmost
            // slot.
            for (proto::proto_ulong i = 0; i < argc; ++i) {
                compileForm(ctx, lst->getAt(ctx, (int)(i + 1)), out, markers);
            }
            for (int i = static_cast<int>(argc) - 1; i >= 0; --i) {
                int slot = tgt.slots[i];
                out.emit(Op::STORE_LOCAL, slot);
            }
            // JUMP_BACK: operand is the instruction count to subtract from
            // pc (pc was at the instruction AFTER JUMP_BACK when handler
            // runs — same arithmetic as JUMP / JUMP_IF_FALSE but in reverse).
            std::size_t jbAt = out.emit(Op::JUMP_BACK, 0);
            std::size_t backOffsetInstr = (jbAt + 1) - tgt.bodyStart;
            out.patchOperand(jbAt,
                backOffsetInstr);
            // recur never falls through; the bytecode after it is unreachable.
            return;
        }

        // ---- Plain call form -----------------------------------------------

        // Session 11 — SmallInt fast-path emission. For the standard
        // operator names AND when the symbol is NOT shadowed by a local
        // binding, we emit binary-fold opcodes (ADD / SUB / MUL / LT /
        // LE / GT / GE / EQ) directly. The VM short-circuits when both
        // operands are tagged SmallInt; otherwise it falls back to the
        // primitive on globals — same semantics as PUSH_VAR + CALL.
        if (isNameForm(ctx, markers.named, head)) {
            Op binOp = Op::NOP;
            if      (headName == "+")  binOp = Op::ADD;
            else if (headName == "-")  binOp = Op::SUB;
            else if (headName == "*")  binOp = Op::MUL;
            else if (headName == "<")  binOp = Op::LT;
            else if (headName == "<=") binOp = Op::LE;
            else if (headName == ">")  binOp = Op::GT;
            else if (headName == ">=") binOp = Op::GE;
            else if (headName == "=")  binOp = Op::EQ;

            // Shadow check — if `+` (or whichever) is bound locally, we
            // are calling the local binding, not the global primitive;
            // fall back to the general PUSH_VAR + CALL path.
            int shadowSlot = (binOp != Op::NOP && !coreQualified) ? resolveLocal(headName) : -1;
            proto::proto_ulong argc = n - 1;

            if (binOp != Op::NOP && shadowSlot < 0 && argc >= 2) {
                // Arithmetic: left-fold pairs. (op a b c d) emits
                // a; b; OP; c; OP; d; OP. Comparisons fold the same way
                // semantically — (< a b c) becomes ((< a b) && (< b c))
                // in Clojure, but our SmallInt fast-path opcodes are
                // strictly binary; for >2 args on comparisons, fall
                // through to the global primitive (correct semantics).
                bool isCompare = (binOp == Op::LT || binOp == Op::LE ||
                                  binOp == Op::GT || binOp == Op::GE ||
                                  binOp == Op::EQ);
                if (isCompare && argc != 2) {
                    // Defer to general path for chained comparisons.
                } else {
                    compileForm(ctx, lst->getAt(ctx, 1), out, markers);
                    for (proto::proto_ulong i = 2; i < n; ++i) {
                        compileForm(ctx, lst->getAt(ctx, (int)i), out, markers);
                        out.emit(binOp, 0);
                    }
                    return;
                }
            }

            // General path — emit callable, then args, then CALL. A keyword
            // in call position is a value, not a global name: `(:a m)`
            // pushes the keyword and the VM calls it as a map lookup.
            if (coreQualified) {
                // The built-in, under the key a `def` does not replace.
                out.emit(Op::PUSH_VAR, out.addSymbol(ctx, "clojure.core/" + headName));
            } else if (isUserQualified(headName)) {
                // `user/f`: the program's global f, never a local.
                out.emit(Op::PUSH_VAR, out.addSymbol(ctx, headName.substr(5)));
            } else if (headName[0] == ':') {
                compileForm(ctx, head, out, markers);
            } else if (int slot = resolveLocal(headName); slot >= 0) {
                out.emit(Op::PUSH_LOCAL, slot);
            } else if (isQualifiedName(headName)) {
                // `(alias/member args...)`: a call of a module member with
                // the module as the receiver (CALL_MEMBER), resolved at run
                // time. Arguments are positional.
                compileQualifiedPrefix(ctx, headName, out);
                for (proto::proto_ulong i = 1; i < n; ++i) {
                    compileForm(ctx, lst->getAt(ctx, static_cast<int>(i)), out, markers);
                }
                out.emit(Op::CALL_MEMBER, n - 1);
                out.emit(Op::NOP, out.addSymbol(ctx, headName.substr(headName.find('/') + 1)));
                return;
            } else {
                std::size_t headIdx = out.addSymbol(ctx, headName);
                out.emit(Op::PUSH_VAR, headIdx);
            }
        } else {
            compileForm(ctx, head, out, markers);
        }

        // Session 14 — trailing kv-pair detection. Walking backwards
        // from the last arg in steps of 2, every position that should
        // host a key must be a keyword. When such a suffix exists the call
        // is emitted as CALL_KW instead of CALL. Every argument is pushed
        // as a plain positional in source order either way: the VM passes
        // them unchanged to ordinary callees and folds the trailing pairs
        // into a kwArgs map only for a keyword-argument callee, whose
        // arity decides where the pairs start. Arguments never go through
        // a map for an ordinary callee, so their order and any repeated
        // keywords are preserved.
        bool hasKvSuffix = false;
        for (long long i = (long long)n - 2; i >= 1; i -= 2) {
            const proto::ProtoObject* arg =
                lst->getAt(ctx, static_cast<int>(i));
            if (!isKeywordForm(ctx, markers.named, arg)) break;
            hasKvSuffix = true;
        }

        for (proto::proto_ulong i = 1; i < n; ++i) {
            compileForm(ctx, lst->getAt(ctx, static_cast<int>(i)), out, markers);
        }

        proto::proto_ulong callArgc = n - 1;
        out.emit(hasKvSuffix ? Op::CALL_KW : Op::CALL,
                 callArgc);
        return;
    }

    // `{..}` map literal in expression position: a call to the `hash-map`
    // primitive with every key and value form, in source order (SourceInfo.h),
    // so they are evaluated left to right as in Clojure.
    if (isMap(form)) {
        const std::vector<const proto::ProtoObject*> entries = mapFormEntries(ctx, form);
        std::size_t headIdx = out.addSymbol(ctx, "hash-map");
        out.emit(Op::PUSH_VAR, headIdx);
        for (const proto::ProtoObject* e : entries) compileForm(ctx, e, out, markers);
        out.emit(Op::CALL, entries.size());
        return;
    }

    // `[..]` vector literal in expression position: a call to the `vector`
    // primitive with every item form, so the items are evaluated.
    if (isVector(form)) {
        const proto::ProtoList* items = vectorItems(ctx, form);
        proto::proto_ulong ni = items->getSize(ctx);
        std::size_t headIdx = out.addSymbol(ctx, "vector");
        out.emit(Op::PUSH_VAR, headIdx);
        for (proto::proto_ulong i = 0; i < ni; ++i) {
            compileForm(ctx, items->getAt(ctx, (int)i), out, markers);
        }
        out.emit(Op::CALL, ni);
        return;
    }

    // `#{..}` set literal in expression position. A set of constants
    // (keywords, strings, numbers, nil, booleans) is the very set the reader
    // built, pinned by the module as a quoted datum is (M5), as Clojure
    // compiles a constant set; any other is a call to hash-set with every
    // element form, evaluated in the set's (unspecified) order.
    if (isSet(ctx, form)) {
        std::vector<const proto::ProtoObject*> elems;
        setForEach(ctx, form, &elems,
            [](proto::ProtoContext*, void* self, const proto::ProtoObject* e) {
                static_cast<std::vector<const proto::ProtoObject*>*>(self)->push_back(e);
            });
        bool constant = true;
        for (const proto::ProtoObject* e : elems) {
            if (!(e == PROTO_NONE || e == PROTO_TRUE || e == PROTO_FALSE || isStringForm(e) ||
                  e->isInteger(ctx) || e->isFloat(ctx) || isKeywordForm(ctx, markers.named, e)))
                constant = false;
        }
        if (constant) {
            out.emit(Op::PUSH_CONST, out.addDatum(ctx, form));
            return;
        }
        out.emit(Op::PUSH_VAR, out.addSymbol(ctx, "clojure.core/hash-set"));
        for (const proto::ProtoObject* e : elems) compileForm(ctx, e, out, markers);
        out.emit(Op::CALL, elems.size());
        return;
    }

    // String literal — a protoCore string, pushed from the const pool.
    if (isStringForm(form)) {
        std::size_t idx = out.addString(
            reinterpret_cast<const proto::ProtoString*>(form)->toStdString(ctx));
        out.emit(Op::PUSH_CONST, idx);
        return;
    }

    // Symbol or keyword atom — keyword literal, local reference, or runtime
    // global lookup, in that order.
    if (isNameForm(ctx, markers.named, form)) {
        std::string name = nameOf(ctx, markers.named, form);
        if (!name.empty() && name[0] == ':') {
            // The keyword is the interned value the reader yielded;
            // PUSH_CONST pushes the pointer (Named.h).
            std::size_t idx = out.addNamed(name, form);
            out.emit(Op::PUSH_CONST, idx);
            return;
        }
        if (std::string bare = name; stripCoreQualifier(bare)) {
            // `clojure.core/x`: the built-in x, never a local, under the key a
            // `def` of x does not replace (Macros.h, defineBuiltin).
            if (isSpecialFormName(bare) || bare == "unquote" || bare == "unquote-splicing")
                throw CompileError("Can't take value of a special form: " + bare);
            if (const ActiveCallContext* cc = activeCallContext(); cc && globalMacro(ctx, *cc, name))
                throw CompileError("Can't take value of a macro: " + name);
            out.emit(Op::PUSH_VAR, out.addSymbol(ctx, name));
            return;
        }
        if (isUserQualified(name)) {
            // `user/x`: the program's global x, never a local.
            if (const ActiveCallContext* cc = activeCallContext(); cc && globalMacro(ctx, *cc, name.substr(5)))
                throw CompileError("Can't take value of a macro: " + name);
            out.emit(Op::PUSH_VAR, out.addSymbol(ctx, name.substr(5)));
            return;
        }
        int slot = resolveLocal(name);
        if (slot >= 0) {
            out.emit(Op::PUSH_LOCAL, slot);
            return;
        }
        if (const ActiveCallContext* cc = activeCallContext()) {
            std::string key;
            if (globalKeyForSymbol(name, key) && globalMacro(ctx, *cc, key)) {
                throw CompileError("Can't take value of a macro: " + name);
            }
        }
        if (isQualifiedName(name)) {
            // `alias/member`: the member of the value of `alias` (a module
            // imported with import-module), read at run time.
            compileQualifiedPrefix(ctx, name, out);
            out.emit(Op::GET_MEMBER, out.addSymbol(ctx, name.substr(name.find('/') + 1)));
            return;
        }
        std::size_t idx = out.addSymbol(ctx, name);
        out.emit(Op::PUSH_VAR, idx);
        return;
    }

    throw CompileError("compile: unsupported form");
}

} // namespace protoClojure
