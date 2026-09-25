/*
 * protoclj — command-line entry point.
 *
 * v0.0.x scope: --version, --help, and *running a .clj file* through the
 * reader/compiler/VM pipeline. Subsequent sessions add -e (one-form eval),
 * the interactive REPL, and --nrepl.
 */
#include "protoClojure.h"
#include "protoCore.h"

#include "reader/Reader.h"
#include "compiler/Compiler.h"
#include "repl/Repl.h"
#include "runtime/ActorScheduler.h"
#include "runtime/BytecodeModule.h"
#include "runtime/ExecutionEngine.h"
#include "runtime/GCCensus.h"
#include "runtime/Named.h"
#include "runtime/Primitives.h"
#include "runtime/StackGuard.h"

#include <cstring>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace protoClojure {

const char* versionString() {
    static const std::string s =
        std::to_string(kVersionMajor) + "." +
        std::to_string(kVersionMinor) + "." +
        std::to_string(kVersionPatch);
    return s.c_str();
}

} // namespace protoClojure

namespace {

void printVersion() {
    std::printf("protoClojure %s\n", protoClojure::versionString());
}

void printHelp() {
    std::printf(
        "Usage: protoclj [options] [script.clj]\n"
        "\n"
        "Options:\n"
        "  --version       Print version and exit.\n"
        "  --help, -h      Print this help and exit.\n"
        "\n"
        "Interactive:\n"
        "  (no args)       Start the interactive REPL (libreadline).\n"
        "\n"
        "Not yet implemented:\n"
        "  -e <expr>       Evaluate one expression (planned).\n"
        "  --nrepl PORT    Start an nREPL server (planned for v0.1).\n");
}

std::string slurp(const char* path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error(std::string("cannot open: ") + path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Run a .clj file end-to-end: read every top-level form, compile each into a
// shared BytecodeModule with statement-level POP separators, terminate with
// RETURN, then execute. Returns the program's exit code.
int runFile(const char* path) {
    proto::ProtoSpace space;
    proto::ProtoContext* ctx = space.rootContext;

    // Declared after the space so it is joined and printed BEFORE the space it
    // samples is destroyed. Inert unless PROTOCLJ_GC_STATS=1.
    protoClojure::GCCensus gcCensus(space);

    // Slots layout (kept stable for the whole run, rooted via the root ctx):
    //   0 : globals namespace (mutable child of objectPrototype).
    //   1 : forms (the ProtoList readAll() returned).
    //   2 : stringMarkerProto — see ReaderMarkers / CompilerMarkers docs.
    //   3 : fnMarkerProto — wraps user-fn callables.
    ctx->resizeAutomaticLocals(13);
    constexpr unsigned int kSlotGlobals       = 0;
    constexpr unsigned int kSlotForms         = 1;
    constexpr unsigned int kSlotStringMarker  = 2;
    constexpr unsigned int kSlotFnSingle      = 3;
    constexpr unsigned int kSlotVectorMarker  = 4;
    constexpr unsigned int kSlotFnMulti       = 5;
    constexpr unsigned int kSlotMapMarker     = 6;
    constexpr unsigned int kSlotAtomMarker    = 7;
    constexpr unsigned int kSlotFutureMarker  = 8;
    constexpr unsigned int kSlotPromiseMarker = 9;
    constexpr unsigned int kSlotActorMarker   = 10;
    constexpr unsigned int kSlotNamedMarker   = 11;
    constexpr unsigned int kSlotNamedTable    = 12;

    const proto::ProtoObject* globalsObj =
        space.objectPrototype->newChild(ctx, /*isMutable=*/true);
    ctx->setAutomaticLocal(kSlotGlobals, globalsObj);

    protoClojure::installPrimitives(
        ctx, const_cast<proto::ProtoObject*>(
            ctx->getAutomaticLocal(kSlotGlobals)));

    // The prototype markers below are never mutated, so they are immutable
    // objects. Only the globals namespace and the Named intern table are
    // mutated in place, and only they are mutable; the atoms, futures,
    // promises and actors created later are mutable children of immutable
    // markers.
    const proto::ProtoObject* stringMarkerProto =
        space.objectPrototype->newChild(ctx);
    ctx->setAutomaticLocal(kSlotStringMarker, stringMarkerProto);

    // Session 12 — single-arity and multi-arity wrappers use DIFFERENT
    // prototypes so the CALL dispatcher picks the path via getPrototype
    // alone, without an `__arities__` probe per call.
    const proto::ProtoObject* fnSingleProto =
        space.objectPrototype->newChild(ctx);
    ctx->setAutomaticLocal(kSlotFnSingle, fnSingleProto);

    const proto::ProtoObject* fnMultiProto =
        space.objectPrototype->newChild(ctx);
    ctx->setAutomaticLocal(kSlotFnMulti, fnMultiProto);

    const proto::ProtoObject* vectorMarkerProto =
        space.objectPrototype->newChild(ctx);
    ctx->setAutomaticLocal(kSlotVectorMarker, vectorMarkerProto);

    const proto::ProtoObject* mapMarkerProto =
        space.objectPrototype->newChild(ctx);
    ctx->setAutomaticLocal(kSlotMapMarker, mapMarkerProto);

    // Session 16 — atoms. Mutable child of atomMarkerProto carrying
    // the current value under `__value__`. CAS via setAttributeIfEqual.
    const proto::ProtoObject* atomMarkerProto =
        space.objectPrototype->newChild(ctx);
    ctx->setAutomaticLocal(kSlotAtomMarker, atomMarkerProto);

    // Session 17 — futures. Mutable child carrying the thunk, the
    // running thread, and the eventually-realised result.
    const proto::ProtoObject* futureMarkerProto =
        space.objectPrototype->newChild(ctx);
    ctx->setAutomaticLocal(kSlotFutureMarker, futureMarkerProto);

    // Session 18 — promises. Same shape as a small atom, with a
    // single-shot CAS deliver and a busy-wait deref.
    const proto::ProtoObject* promiseMarkerProto =
        space.objectPrototype->newChild(ctx);
    ctx->setAutomaticLocal(kSlotPromiseMarker, promiseMarkerProto);

    // Session 19 — actors. Mutable wrapper child carrying the
    // current value and an opaque handle to the C++ ActorState owned
    // by the global ActorScheduler. Worker pool spawns on the first
    // (actor ...) call.
    const proto::ProtoObject* actorMarkerProto =
        space.objectPrototype->newChild(ctx);
    ctx->setAutomaticLocal(kSlotActorMarker, actorMarkerProto);

    // Keywords and symbols: the prototype of every named value and the
    // table that interns them by spelling (src/runtime/Named.h).
    const proto::ProtoObject* namedMarkerProto =
        space.objectPrototype->newChild(ctx);
    ctx->setAutomaticLocal(kSlotNamedMarker, namedMarkerProto);
    const proto::ProtoObject* namedTable =
        space.objectPrototype->newChild(ctx, /*isMutable=*/true);
    ctx->setAutomaticLocal(kSlotNamedTable, namedTable);

    const proto::ProtoString* bytesKey =
        proto::ProtoString::createSymbol(ctx, "__bytes__");
    const proto::ProtoString* bytecodeKey =
        proto::ProtoString::createSymbol(ctx, "__bytecode__");
    const proto::ProtoString* arityKey =
        proto::ProtoString::createSymbol(ctx, "__arity__");
    const proto::ProtoString* capturesKey =
        proto::ProtoString::createSymbol(ctx, "__captures__");
    const proto::ProtoString* aritiesKey =
        proto::ProtoString::createSymbol(ctx, "__arities__");
    const proto::ProtoString* itemsKey =
        proto::ProtoString::createSymbol(ctx, "__items__");
    // `__entries__` holds the source-order entries of a map literal between
    // the Reader and the Compiler. A runtime map is a ProtoMap with no
    // attributes (src/runtime/MapOps.h).
    const proto::ProtoString* entriesKey =
        proto::ProtoString::createSymbol(ctx, "__entries__");
    const proto::ProtoString* valueKey =
        proto::ProtoString::createSymbol(ctx, "__value__");
    const proto::ProtoString* watchesKey =
        proto::ProtoString::createSymbol(ctx, "__watches__");
    const proto::ProtoString* thunkKey =
        proto::ProtoString::createSymbol(ctx, "__thunk__");
    const proto::ProtoString* ccBlobKey =
        proto::ProtoString::createSymbol(ctx, "__cc_blob__");
    const proto::ProtoString* threadKey =
        proto::ProtoString::createSymbol(ctx, "__thread__");
    const proto::ProtoString* resultKey =
        proto::ProtoString::createSymbol(ctx, "__result__");
    const proto::ProtoString* doneKey =
        proto::ProtoString::createSymbol(ctx, "__done__");
    const proto::ProtoString* actorStateKey =
        proto::ProtoString::createSymbol(ctx, "__actor_state__");
    const proto::ProtoString* mailboxKey =
        proto::ProtoString::createSymbol(ctx, "__mailbox__");
    const protoClojure::NamedLayout namedLayout{
        ctx->getAutomaticLocal(kSlotNamedMarker),
        ctx->getAutomaticLocal(kSlotNamedTable),
        proto::ProtoString::createSymbol(ctx, "__spelling__")};

    protoClojure::ReaderMarkers readerMarkers{
        ctx->getAutomaticLocal(kSlotStringMarker),
        ctx->getAutomaticLocal(kSlotVectorMarker),
        ctx->getAutomaticLocal(kSlotMapMarker),
        bytesKey, itemsKey, entriesKey};
    protoClojure::CompilerMarkers compilerMarkers{
        ctx->getAutomaticLocal(kSlotStringMarker),
        ctx->getAutomaticLocal(kSlotVectorMarker),
        ctx->getAutomaticLocal(kSlotMapMarker),
        bytesKey, bytecodeKey, arityKey, capturesKey, aritiesKey,
        itemsKey, entriesKey, namedLayout};

    // Read every form from the file.
    std::string source = slurp(path);
    protoClojure::Reader reader(ctx, std::move(source), readerMarkers);
    try {
        const proto::ProtoList* forms = reader.readAll();
        ctx->setAutomaticLocal(kSlotForms, forms->asObject(ctx));
    } catch (const protoClojure::ReaderError& e) {
        std::fprintf(stderr, "%s:%d:%d: read error: %s\n",
                     path, e.line, e.column, e.what());
        return 1;
    }

    protoClojure::Compiler compiler;
    protoClojure::BytecodeModule mod;

    const proto::ProtoList* forms =
        ctx->getAutomaticLocal(kSlotForms)->asList(ctx);
    unsigned long n = forms->getSize(ctx);
    for (unsigned long i = 0; i < n; ++i) {
        const proto::ProtoObject* form =
            forms->getAt(ctx, static_cast<int>(i));
        try {
            compiler.compileForm(ctx, form, mod, compilerMarkers);
        } catch (const std::exception& e) {
            // A CompileError, or a limit the bytecode itself enforces
            // (BytecodeModule::emit).
            std::fprintf(stderr, "%s: compile error: %s\n", path, e.what());
            return 1;
        }
        // Statement-level discard, except for the last form whose value is
        // the file's value.
        if (i + 1 < n) mod.emit(protoClojure::Op::POP, 0);
    }
    mod.emit(protoClojure::Op::RETURN, 0);

    protoClojure::ExecutionEngine eng;
    try {
        eng.run(ctx, mod, ctx->getAutomaticLocal(kSlotGlobals),
                ctx->getAutomaticLocal(kSlotFnSingle),
                ctx->getAutomaticLocal(kSlotFnMulti),
                ctx->getAutomaticLocal(kSlotAtomMarker),
                ctx->getAutomaticLocal(kSlotFutureMarker),
                ctx->getAutomaticLocal(kSlotPromiseMarker),
                ctx->getAutomaticLocal(kSlotActorMarker),
                bytecodeKey, arityKey, capturesKey, aritiesKey,
                valueKey, watchesKey,
                thunkKey, ccBlobKey, threadKey, resultKey, doneKey,
                actorStateKey, mailboxKey, namedLayout);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s: runtime error: %s\n", path, e.what());
        // Drain the actor scheduler before unwinding so worker
        // threads don't outlive the ProtoSpace.
        protoClojure::shutdownFutures(ctx);
        protoClojure::ActorScheduler::instance().shutdown(ctx);
        return 1;
    }
    // Graceful shutdown — wait for future + actor workers to drain
    // pending work, then join. Without this, a script that fires off
    // futures or actor sends without explicit @ may segfault at exit
    // when the ProtoSpace destructor runs while workers still hold
    // pointers. Same root cause for both, same fix: join first.
    protoClojure::shutdownFutures(ctx);
    protoClojure::ActorScheduler::instance().shutdown(ctx);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    // Every non-tail call nests the VM on the native stack, so the stack
    // size bounds the recursion depth. The evaluator runs on a thread with
    // a stack of protoClojure::kThreadStackBytes, and every thread created
    // later (futures, pmap, actor workers) gets the same size by default;
    // the VM raises StackOverflowError before any of them overflows.
    protoClojure::configureThreadStacks();

    if (argc < 2) {
        // No arguments → drop into the interactive REPL.
        return protoClojure::runOnEvaluatorThread(
            [](void*) { return protoClojure::runRepl(); }, nullptr);
    }

    // Walk argv. A non-flag argument is treated as a .clj file to run.
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--version") == 0) {
            printVersion();
            return 0;
        }
        if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
            printHelp();
            return 0;
        }
        if (a[0] == '-') {
            std::fprintf(stderr,
                "protoclj: unknown flag '%s'. Try --help.\n", a);
            return 1;
        }
        // Positional: a file to run.
        return protoClojure::runOnEvaluatorThread(
            [](void* path) { return runFile(static_cast<const char*>(path)); },
            static_cast<void*>(argv[i]));
    }
    return 0;
}
