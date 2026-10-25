#include "Eval.h"

#include "Compiler.h"

#include "reader/Reader.h"
#include "reader/SourceInfo.h"

#include "runtime/BytecodeModule.h"
#include "runtime/DebugHook.h"
#include "runtime/Exceptions.h"
#include "runtime/ExecutionEngine.h"
#include "runtime/Macros.h"
#include "runtime/Opcodes.h"
#include "runtime/Primitives.h"
#include "runtime/SeqTypes.h"

#include "protoCore.h"

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace protoClojure {

namespace {

std::mutex gModulesMutex;
std::vector<std::unique_ptr<BytecodeModule>>& retained() {
    static std::vector<std::unique_ptr<BytecodeModule>> modules;
    return modules;
}

// True when `form` is `(do ...)` or `(clojure.core/do ...)`.
bool isDoForm(proto::ProtoContext* ctx, const NamedLayout& named,
              const proto::ProtoObject* form) {
    if (!isListValue(ctx, form) || form == emptyListValue()) return false;
    const proto::ProtoObject* head = listFirst(ctx, form);
    return head == internNamed(ctx, named, "do") ||
           head == internNamed(ctx, named, "clojure.core/do");
}

const proto::ProtoObject* evalHookEntry(proto::ProtoContext* ctx,
                                        const proto::ProtoObject* form) {
    try {
        return evalTopLevel(ctx, form, nullptr);
    } catch (const CompileError& e) {
        throwClassed(ctx, "RuntimeException", std::string("Syntax error compiling: ") + e.what());
    }
}

} // namespace

void retainModule(std::unique_ptr<BytecodeModule> mod) {
    std::lock_guard<std::mutex> lock(gModulesMutex);
    retained().push_back(std::move(mod));
}

void releaseRetainedModules() {
    std::vector<std::unique_ptr<BytecodeModule>> doomed;
    {
        std::lock_guard<std::mutex> lock(gModulesMutex);
        doomed.swap(retained());
    }
}

void installEval() {
    setEvalHook(&evalHookEntry);
    setSyntaxQuoteResolverHook(&qualifySyntaxQuoteSymbol);
}

namespace {
const char kCoreSource[] = {
#include "core_clj.inc"
    0};
} // namespace

const std::string& embeddedCoreSource() {
    static const std::string text(kCoreSource, sizeof kCoreSource - 1);
    return text;
}

const char* embeddedCoreSourceName() { return "protoclj:/clojure/core.clj"; }

bool loadCore(proto::ProtoContext* ctx) {
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) {
        std::fprintf(stderr, "core.clj: no evaluator is running\n");
        return false;
    }
    setCoreNamespaceActive(true);
    struct Reset { ~Reset() { setCoreNamespaceActive(false); } } reset;
    Reader reader(ctx, embeddedCoreSource(), cc->named);
    // Under the debugger clojure.core is compiled with debug information as
    // library code: its fns can be stepped into and carry breakpoints.
    const bool debugging = debugHook() != nullptr;
    reader.setSymbolResolver(&qualifySyntaxQuoteSymbol);
    for (;;) {
        proto::ProtoContext formScope(ctx->space, ctx);
        formScope.resizeAutomaticLocals(1);
        SourceInfo sourceInfo;
        if (debugging) {
            sourceInfo.recordPositions = true;
            sourceInfo.sourceName = embeddedCoreSourceName();
            sourceInfo.library = true;
        }
        reader.setSourceInfo(&sourceInfo);
        try {
            const proto::ProtoObject* form = reader.readOne(&formScope);
            reader.setSourceInfo(nullptr);
            if (!form) return true;
            formScope.setAutomaticLocal(0, form);
            evalTopLevel(&formScope, formScope.getAutomaticLocal(0), &sourceInfo);
        } catch (const ReaderError& e) {
            std::fprintf(stderr, "core.clj:%d:%d: read error: %s\n", e.line, e.column, e.what());
            return false;
        } catch (const CompileError& e) {
            std::fprintf(stderr, "core.clj:%d:%d: compile error: %s\n",
                         reader.formLine(), reader.formColumn(), e.what());
            return false;
        } catch (const proto::ProtoException& e) {
            std::fprintf(stderr, "core.clj: runtime error: %s\n",
                         describeThrown(&formScope, e.value()).c_str());
            return false;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "core.clj: runtime error: %s\n", e.what());
            return false;
        }
    }
}

std::string qualifySyntaxQuoteSymbol(proto::ProtoContext* ctx, const std::string& name) {
    // Clojure's special forms (Compiler.specials) are never qualified.
    static const std::unordered_set<std::string> kSpecials = {
        "def", "loop*", "recur", "if", "case*", "let*", "letfn*", "do", "fn*",
        "quote", "var", "import*", ".", "set!", "deftype*", "reify*", "try",
        "throw", "monitor-enter", "monitor-exit", "catch", "finally", "new", "&"};
    // The forms protoClojure's compiler implements itself, which are
    // clojure.core macros in JVM Clojure.
    static const std::unordered_set<std::string> kCompilerForms = {
        "let", "loop", "fn", "defn", "defmacro", "and", "or", "apply"};
    if (kSpecials.count(name)) return name;
    if (name == "/") return "clojure.core//";
    if (name.find('/') != std::string::npos) return name;
    if (name.find('.') != std::string::npos) return name;
    if (exceptionClassId(name) >= 0 || name == "Object") return name;
    if (kCompilerForms.count(name)) return "clojure.core/" + name;
    // core.clj's own templates: its namespace is clojure.core.
    if (coreNamespaceActive()) return "clojure.core/" + name;
    const ActiveCallContext* cc = activeCallContext();
    if (!cc || !cc->globals) return name;
    const std::string core = "clojure.core/" + name;
    const proto::ProtoString* coreKey = internSymbol(ctx, cc->named, core);
    if (cc->globals->hasOwnAttribute(ctx, coreKey) == PROTO_TRUE) {
        // A built-in, unless the program has defined the name itself.
        const proto::ProtoString* userKey = internSymbol(ctx, cc->named, name);
        if (cc->globals->hasOwnAttribute(ctx, userKey) != PROTO_TRUE ||
            cc->globals->getAttribute(ctx, userKey) == cc->globals->getAttribute(ctx, coreKey))
            return core;
    }
    return "user/" + name;
}

const proto::ProtoObject* evalTopLevel(proto::ProtoContext* ctx,
                                       const proto::ProtoObject* form,
                                       const SourceInfo* info) {
    const ActiveCallContext* cc = activeCallContext();
    if (!cc || !cc->engine) throw CompileError("eval: no evaluator is running on this thread");
    const CompilerMarkers markers{cc->bytecodeKey, cc->arityKey, cc->capturesKey,
                                  cc->aritiesKey, cc->named};

    // Slot 0: the form, then its expansion; slot 1: the last result.
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(2);
    scope.setAutomaticLocal(0, form);
    {
        Compiler expander;
        try {
            scope.setAutomaticLocal(0, expander.macroexpandTopLevel(&scope, form, markers));
        } catch (const CompileError&) {
            throw;
        } catch (const proto::ProtoException& e) {
            throw CompileError(describeThrown(&scope, e.value()));
        } catch (const std::exception& e) {
            // StackOverflowError (a macro expanding into itself) or a
            // bytecode limit: an error of the compile phase.
            throw CompileError(e.what());
        }
    }

    const proto::ProtoObject* expanded = scope.getAutomaticLocal(0);
    if (isDoForm(&scope, cc->named, expanded)) {
        // A top-level do: each form is expanded, compiled and run before the
        // next is compiled, as in Clojure.
        // The forms are cells of `expanded`, which slot 0 roots.
        scope.setAutomaticLocal(1, PROTO_NONE);
        for (const proto::ProtoObject* p = listRest(&scope, expanded); p != emptyListValue();
             p = listRest(&scope, p)) {
            scope.setAutomaticLocal(1, evalTopLevel(&scope, listFirst(&scope, p), info));
        }
        scope.returnValue = scope.getAutomaticLocal(1);
        return scope.getAutomaticLocal(1);
    }

    auto modPtr = std::make_unique<BytecodeModule>();
    BytecodeModule& mod = *modPtr;
    if (info && info->recordPositions) {
        // Debugging (docs/debugging.md): the form's own module is a frame too.
        BytecodeModule::DebugInfo& di = mod.ensureDebugInfo();
        di.source = info->sourceName;
        di.name = "<top level>";
        di.library = info->library;
    }
    {
        Compiler compiler;
        compiler.setSourceInfo(info);
        try {
            compiler.compileForm(&scope, expanded, mod, markers);
        } catch (const CompileError&) {
            throw;
        } catch (const proto::ProtoException& e) {
            throw CompileError(describeThrown(&scope, e.value()));
        } catch (const std::exception& e) {
            // StackOverflowError (source nested too deeply) or a bytecode
            // limit (BytecodeModule::emit).
            throw CompileError(e.what());
        }
    }
    mod.emit(Op::RETURN, 0);
    // Retained before it runs: the functions it defines point into it.
    retainModule(std::move(modPtr));

    const proto::ProtoObject* result = cc->engine->run(&scope, mod, cc->globals,
        cc->fnSingleProto, cc->fnMultiProto, cc->atomMarkerProto,
        cc->futureMarkerProto, cc->promiseMarkerProto, cc->actorMarkerProto,
        cc->bytecodeKey, cc->arityKey, cc->capturesKey, cc->aritiesKey,
        cc->valueKey, cc->watchesKey,
        cc->thunkKey, cc->ccBlobKey, cc->threadKey, cc->resultKey, cc->doneKey,
        cc->actorStateKey, cc->mailboxKey, cc->named);
    // Anchored in `ctx` when this scope ends (ProtoContext::returnValue).
    scope.returnValue = result;
    return result;
}

} // namespace protoClojure
