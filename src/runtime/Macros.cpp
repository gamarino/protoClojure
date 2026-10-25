#include "CallerSlots.h"
#include "Macros.h"

#include <unordered_map>

#include <mutex>

#include "Exceptions.h"
#include "ExecutionEngine.h"
#include "Named.h"
#include "LazySeq.h"
#include "Primitives.h"

#include "protoCore.h"

#include <atomic>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace protoClojure {

namespace {

std::atomic<EvalHook> gEvalHook{nullptr};
std::atomic<SyntaxQuoteResolverHook> gResolverHook{nullptr};
std::atomic<bool> gCoreNamespace{false};

} // namespace

const proto::ProtoString* macroFlagKey(proto::ProtoContext* ctx) {
    return proto::ProtoString::createSymbol(ctx, "__macro__");
}

bool isMacroValue(proto::ProtoContext* ctx, const ActiveCallContext& cc,
                  const proto::ProtoObject* v) {
    if (!v || v == PROTO_NONE) return false;
    const proto::ProtoObject* proto = v->getPrototype(ctx);
    if (proto != cc.fnSingleProto && proto != cc.fnMultiProto) return false;
    return v->hasOwnAttribute(ctx, macroFlagKey(ctx)) == PROTO_TRUE;
}

bool isSpecialFormName(const std::string& name) {
    static const std::unordered_set<std::string> kSpecial = {
        "def", "if", "do", "let*", "loop*", "fn*", "quote", "var", "recur",
        "throw", "try", "catch", "finally", "new", "set!", "letfn*", "case*",
        ".", "&", "monitor-enter", "monitor-exit", "import*", "deftype*",
        "reify*",
        // protoClojure compiles these directly, as Clojure compiles let*,
        // loop* and fn*.
        "let", "loop", "fn"};
    return kSpecial.count(name) != 0;
}

bool globalKeyForSymbol(const std::string& spelling, std::string& key) {
    if (spelling.empty() || spelling[0] == ':') return false;
    static const std::string kUser = "user/";
    if (spelling.size() > kUser.size() && spelling.compare(0, kUser.size(), kUser) == 0) {
        key = spelling.substr(kUser.size());
    } else {
        key = spelling;
    }
    return true;
}

void defineBuiltin(proto::ProtoContext* ctx, proto::ProtoObject* globals,
                   const char* name, const proto::ProtoObject* value) {
    globals->setAttribute(ctx, proto::ProtoString::createSymbol(ctx, name), value);
    const std::string core = std::string("clojure.core/") + name;
    globals->setAttribute(ctx, proto::ProtoString::createSymbol(ctx, core.c_str()), value);
}

const proto::ProtoObject* globalMacro(proto::ProtoContext* ctx,
                                      const ActiveCallContext& cc,
                                      const std::string& key) {
    if (!cc.globals) return nullptr;
    const proto::ProtoString* k = internSymbol(ctx, cc.named, key);
    if (cc.globals->hasOwnAttribute(ctx, k) != PROTO_TRUE) return nullptr;
    const proto::ProtoObject* v = cc.globals->getAttribute(ctx, k);
    return isMacroValue(ctx, cc, v) ? v : nullptr;
}

const proto::ProtoObject* expandMacroCall(proto::ProtoContext* ctx,
                                          const ActiveCallContext& cc,
                                          const proto::ProtoObject* macro,
                                          const std::string& name,
                                          const proto::ProtoObject* form,
                                          const proto::ProtoObject* env) {
    // The arguments are the call's own forms after its head: the cells of a
    // list, or the elements of any other seq a macro built (realised, and
    // held by `form`). All are reachable from `form`, which the caller
    // roots; invoke copies them into the macro's frame.
    std::vector<const proto::ProtoObject*> args;
    args.push_back(form);
    args.push_back(env ? env : PROTO_NONE);
    if (isListValue(ctx, form)) {
        ListCursor c(form);
        c.next();  // the macro's name
        while (c.hasNext()) args.push_back(c.next());
    } else {
        CallerSlots scope(ctx, 1);
        SeqWalk w(scope, 0, ctx, form);
        if (!w.done()) w.advance(ctx);
        for (; !w.done(); w.advance(ctx)) args.push_back(w.first(ctx));
    }
    const proto::proto_ulong n = static_cast<proto::proto_ulong>(args.size() - 1);
    const unsigned int argc = static_cast<unsigned int>(args.size());
    if (!cc.engine->acceptsArgCount(ctx, macro, argc)) {
        throwClassed(ctx, "ArityException",
                     "Wrong number of args (" + std::to_string(n - 1) +
                     ") passed to macro: " + name);
    }
    return cc.engine->invoke(ctx, macro, args.data(), argc);
}

const proto::ProtoObject* macroexpandOnce(proto::ProtoContext* ctx,
                                          const ActiveCallContext& cc,
                                          const proto::ProtoObject* form) {
    // A seq that is not a list is read as the list of its elements, as
    // Clojure's macroexpand-1 reads any ISeq; the list is anchored in `ctx`.
    if (isSeqObject(ctx, form) && !isListValue(ctx, form))
        form = listFromProtoList(ctx, seqToList(ctx, form));
    if (!isListValue(ctx, form) || form == emptyListValue()) return form;
    const proto::ProtoObject* head = listFirst(ctx, form);
    if (!isNamed(ctx, cc.named, head)) return form;
    const std::string spelling = namedSpellingText(ctx, cc.named, head);
    std::string key;
    if (!globalKeyForSymbol(spelling, key) || isSpecialFormName(key)) return form;
    const proto::ProtoObject* macro = globalMacro(ctx, cc, key);
    if (!macro) return form;
    return expandMacroCall(ctx, cc, macro, spelling, form, PROTO_NONE);
}

namespace {
std::mutex gAliasMutex;
std::unordered_map<std::string, std::string> gAliases;
}

void addNamespaceAlias(const std::string& alias, const std::string& ns) {
    std::lock_guard<std::mutex> lock(gAliasMutex);
    gAliases[alias] = ns;
}

bool namespaceAlias(const std::string& alias, std::string& ns) {
    std::lock_guard<std::mutex> lock(gAliasMutex);
    auto it = gAliases.find(alias);
    if (it == gAliases.end()) return false;
    ns = it->second;
    return true;
}

bool isKnownNamespace(const std::string& ns) {
    return ns == "clojure.core" || ns == "clojure.string" || ns == "clojure.set" ||
           ns == "clojure.java.io" || ns == "clojure.java.shell" || ns == "clojure.edn" ||
           ns == "cheshire.core" || ns == "clojure.data.json";
}

bool coreNamespaceActive() { return gCoreNamespace.load(std::memory_order_relaxed); }
void setCoreNamespaceActive(bool active) { gCoreNamespace.store(active, std::memory_order_relaxed); }

void setEvalHook(EvalHook hook) { gEvalHook.store(hook); }
EvalHook evalHook() { return gEvalHook.load(); }

void setSyntaxQuoteResolverHook(SyntaxQuoteResolverHook hook) { gResolverHook.store(hook); }
SyntaxQuoteResolverHook syntaxQuoteResolverHook() { return gResolverHook.load(); }

} // namespace protoClojure
