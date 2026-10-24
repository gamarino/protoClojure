#include "Macros.h"

#include <unordered_map>

#include <mutex>

#include "Exceptions.h"
#include "ExecutionEngine.h"
#include "Named.h"
#include "Primitives.h"

#include "protoCore.h"

#include <atomic>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace protoClojure {

namespace {

bool isListValue(const proto::ProtoObject* v) {
    constexpr std::uintptr_t kTagMask = 0x3F, kTagList = 2, kTagListSmall = 25;
    const std::uintptr_t tag = reinterpret_cast<std::uintptr_t>(v) & kTagMask;
    return v != nullptr && (tag == kTagList || tag == kTagListSmall);
}

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
    // The arguments are the call's own forms, reachable from `form`, which
    // the caller roots; invoke copies them into the macro's frame.
    const proto::ProtoList* lst = form->asList(ctx);
    const proto::proto_ulong n = lst->getSize(ctx);
    std::vector<const proto::ProtoObject*> args;
    args.reserve(n + 1);
    args.push_back(form);
    args.push_back(env ? env : PROTO_NONE);
    for (proto::proto_ulong i = 1; i < n; ++i) args.push_back(lst->getAt(ctx, static_cast<int>(i)));
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
    if (!isListValue(form)) return form;
    const proto::ProtoList* lst = form->asList(ctx);
    if (lst->getSize(ctx) == 0) return form;
    const proto::ProtoObject* head = lst->getAt(ctx, 0);
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
