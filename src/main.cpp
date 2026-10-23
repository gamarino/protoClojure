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
#include "runtime/Exceptions.h"
#include "runtime/ExecutionEngine.h"
#include "runtime/GCCensus.h"
#include "runtime/IO.h"
#include "runtime/Named.h"
#include "runtime/Primitives.h"
#include "runtime/StackGuard.h"

#if defined(PROTOCLJ_EXAMPLE_GEOMETRY)
#include "geometry_module/GeometryModule.h"
#endif

#include <cstring>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#define NOMINMAX
#include <windows.h>
#endif

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
        "Usage: protoclj [options] [script.clj [args...]]\n"
        "\n"
        "Options:\n"
        "  --version       Print version and exit.\n"
        "  --help, -h      Print this help and exit.\n"
        "\n"
        "Arguments after the script's path are bound, as strings, to\n"
        "*command-line-args* (nil when there are none).\n"
        "\n"
        "Interactive:\n"
        "  (no args)       Start the interactive REPL (libreadline).\n"
        "\n"
        "Not yet implemented:\n"
        "  -e <expr>       Evaluate one expression (planned).\n"
        "  --nrepl PORT    Start an nREPL server (planned for v0.1).\n");
}

std::string slurp(const char* path) {
#if defined(_WIN32)
    // Windows has no /dev/stdin; the name reads standard input there too, so
    // `... | protoclj /dev/stdin` works as it does on Linux and macOS.
    if (std::strcmp(path, "/dev/stdin") == 0) {
        std::string all;
        char buf[1 << 16];
        std::size_t got;
        while ((got = std::fread(buf, 1, sizeof buf, stdin)) > 0) all.append(buf, got);
        return all;
    }
#endif
    // Binary: the program's bytes exactly, on Windows too (no CRLF folding).
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error(std::string("cannot open: ") + path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Run a .clj file end-to-end: read every top-level form, compile each into a
// shared BytecodeModule with statement-level POP separators, terminate with
// RETURN, then execute. Returns the program's exit code.
struct ScriptRun {
    const char* path;
    std::vector<std::string> args;  // *command-line-args*
};

// Runs on the evaluator thread; `ctx` is that thread's context.
int runFile(proto::ProtoContext* ctx, const char* path, const std::vector<std::string>& scriptArgs) {
    proto::ProtoSpace& space = *ctx->space;

    // Declared after the space so it is joined and printed BEFORE the space it
    // samples is destroyed. Inert unless PROTOCLJ_GC_STATS=1.
    protoClojure::GCCensus gcCensus(space);

    // Slots layout (kept stable for the whole run, rooted via the root ctx):
    //   0 : globals namespace (mutable child of objectPrototype).
    //   1 : the form being compiled and run.
    //   2.. : the prototypes of fns, atoms, futures, promises, actors and
    //         named values, and the named-value intern table.
    ctx->resizeAutomaticLocals(10);
    constexpr unsigned int kSlotGlobals       = 0;
    constexpr unsigned int kSlotForms         = 1;
    constexpr unsigned int kSlotFnSingle      = 2;
    constexpr unsigned int kSlotFnMulti       = 3;
    constexpr unsigned int kSlotAtomMarker    = 4;
    constexpr unsigned int kSlotFutureMarker  = 5;
    constexpr unsigned int kSlotPromiseMarker = 6;
    constexpr unsigned int kSlotActorMarker   = 7;
    constexpr unsigned int kSlotNamedMarker   = 8;
    constexpr unsigned int kSlotNamedTable    = 9;

    const proto::ProtoObject* globalsObj =
        space.objectPrototype->newChild(ctx, /*isMutable=*/true);
    ctx->setAutomaticLocal(kSlotGlobals, globalsObj);

    protoClojure::installPrimitives(
        ctx, const_cast<proto::ProtoObject*>(
            ctx->getAutomaticLocal(kSlotGlobals)));
    protoClojure::setCommandLineArgs(
        ctx, const_cast<proto::ProtoObject*>(
            ctx->getAutomaticLocal(kSlotGlobals)), scriptArgs);

    // The prototype markers below are never mutated, so they are immutable
    // objects. Only the globals namespace and the Named intern table are
    // mutated in place, and only they are mutable; the atoms, futures,
    // promises and actors created later are mutable children of immutable
    // markers.
    // Session 12 — single-arity and multi-arity wrappers use DIFFERENT
    // prototypes so the CALL dispatcher picks the path via getPrototype
    // alone, without an `__arities__` probe per call.
    const proto::ProtoObject* fnSingleProto =
        space.objectPrototype->newChild(ctx);
    ctx->setAutomaticLocal(kSlotFnSingle, fnSingleProto);

    const proto::ProtoObject* fnMultiProto =
        space.objectPrototype->newChild(ctx);
    ctx->setAutomaticLocal(kSlotFnMulti, fnMultiProto);

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

    const proto::ProtoString* bytecodeKey =
        proto::ProtoString::createSymbol(ctx, "__bytecode__");
    const proto::ProtoString* arityKey =
        proto::ProtoString::createSymbol(ctx, "__arity__");
    const proto::ProtoString* capturesKey =
        proto::ProtoString::createSymbol(ctx, "__captures__");
    const proto::ProtoString* aritiesKey =
        proto::ProtoString::createSymbol(ctx, "__arities__");
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

    protoClojure::CompilerMarkers compilerMarkers{
        bytecodeKey, arityKey, capturesKey, aritiesKey, namedLayout};

    // Read every form from the file.
    std::string source = slurp(path);
    // The forms stay rooted in kSlotForms until the run ends, so one
    // SourceInfo describes them all (SourceInfo.h).
    protoClojure::SourceInfo sourceInfo;
    protoClojure::Reader reader(ctx, std::move(source), namedLayout, &sourceInfo);
    try {
        const proto::ProtoList* forms = reader.readAll();
        ctx->setAutomaticLocal(kSlotForms, forms->asObject(ctx));
    } catch (const protoClojure::ReaderError& e) {
        std::fprintf(stderr, "%s:%d:%d: read error: %s\n",
                     path, e.line, e.column, e.what());
        return 1;
    }

    protoClojure::Compiler compiler;
    compiler.setSourceInfo(&sourceInfo);
    protoClojure::BytecodeModule mod;

    const proto::ProtoList* forms =
        ctx->getAutomaticLocal(kSlotForms)->asList(ctx);
    proto::proto_ulong n = forms->getSize(ctx);
    for (proto::proto_ulong i = 0; i < n; ++i) {
        const proto::ProtoObject* form =
            forms->getAt(ctx, static_cast<int>(i));
        try {
            compiler.compileForm(ctx, form, mod, compilerMarkers);
        } catch (const std::exception& e) {
            // A CompileError, or a limit the bytecode itself enforces
            // (BytecodeModule::emit).
            std::fprintf(stderr, "%s: compile error: %s\n", path, e.what());
            return 1;
        } catch (const proto::ProtoException& e) {
            std::fprintf(stderr, "%s: compile error: %s\n", path,
                         protoClojure::describeThrown(ctx, e.value()).c_str());
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
    } catch (const proto::ProtoException& e) {
        // An uncaught Clojure exception, protoCore error or module error:
        // reported by its value, as JVM Clojure reports an uncaught
        // Throwable (docs/DESIGN.md §4.1).
        // The report prints the exception's data map, and the printer needs
        // a call context (keywords, atoms, ...); the one run installed is
        // gone by now.
        const protoClojure::ActiveCallContext reportCc{&eng,
            ctx->getAutomaticLocal(kSlotGlobals),
            ctx->getAutomaticLocal(kSlotFnSingle), ctx->getAutomaticLocal(kSlotFnMulti),
            ctx->getAutomaticLocal(kSlotAtomMarker), ctx->getAutomaticLocal(kSlotFutureMarker),
            ctx->getAutomaticLocal(kSlotPromiseMarker), ctx->getAutomaticLocal(kSlotActorMarker),
            bytecodeKey, arityKey, capturesKey, aritiesKey, valueKey, watchesKey,
            thunkKey, ccBlobKey, threadKey, resultKey, doneKey,
            actorStateKey, mailboxKey, namedLayout};
        protoClojure::setActiveCallContext(reportCc);
        std::fprintf(stderr, "%s: runtime error: %s\n", path,
                     protoClojure::describeThrown(ctx, e.value()).c_str());
        protoClojure::clearActiveCallContext();
        protoClojure::shutdownIO(ctx);
        protoClojure::shutdownFutures(ctx);
        protoClojure::ActorScheduler::instance().shutdown(ctx);
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s: runtime error: %s\n", path, e.what());
        // Stop HTTP servers and drain the actor scheduler before
        // unwinding so worker threads don't outlive the ProtoSpace.
        protoClojure::shutdownIO(ctx);
        protoClojure::shutdownFutures(ctx);
        protoClojure::ActorScheduler::instance().shutdown(ctx);
        return 1;
    } catch (...) {
        std::fprintf(stderr, "%s: runtime error: unknown C++ exception\n", path);
        protoClojure::shutdownIO(ctx);
        protoClojure::shutdownFutures(ctx);
        protoClojure::ActorScheduler::instance().shutdown(ctx);
        return 1;
    }
    // Graceful shutdown — wait for future + actor workers to drain
    // pending work, then join. Without this, a script that fires off
    // futures or actor sends without explicit @ may segfault at exit
    // when the ProtoSpace destructor runs while workers still hold
    // pointers. Same root cause for both, same fix: join first. HTTP
    // servers still running are stopped first: their handlers may use
    // futures and actors.
    protoClojure::shutdownIO(ctx);
    protoClojure::shutdownFutures(ctx);
    protoClojure::ActorScheduler::instance().shutdown(ctx);
    return 0;
}

} // namespace

namespace {

// Windows: the standard streams carry exactly the bytes the program writes, as
// on Linux and macOS (no "\n" -> "\r\n" translation), and a console shows and
// reads them as UTF-8. The process code page is UTF-8 through the manifest
// (src/windows/utf8.manifest).
void prepareStandardStreams() {
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
}

// The process's memory policy: protoCore's adaptive heap controller
// (protoCore docs/GarbageCollector.md, "Adaptive heap controller"; README.md,
// "Memory"). The heap budget is 75 % of the smaller of physical memory and the
// process memory limit (cgroup on Linux, job object on Windows); a soft limit
// inside it grows only while the program waits for the collector. Memory is
// never returned to the operating system. PROTOCORE_HEAP_LIMIT_CELLS=<hard> or
// <soft>,<hard> sets the budget in cells; PROTOCORE_ADAPTIVE_HEAP=0 applies it
// as a fixed hard limit instead. Built against a protoCore older than 2.10.0
// (the floor is 2.7.0) there is no controller, and protoCore starts no
// collection cycle by itself unless PROTOCORE_HEAP_LIMIT_CELLS is set.
void applyHeapPolicy(proto::ProtoSpace& space) {
#if defined(PROTOCORE_HAS_ADAPTIVE_HEAP)
    space.enableAdaptiveHeap();
#else
    (void)space;
#endif
}

// The test build protoclj_geometry (CMakeLists.txt) links protoCore's example
// geometry module: it registers the module's provider and puts it first in
// the space's resolution chain, so `(import-module "example/geometry")`
// finds it. The ordinary protoclj resolves modules through protoCore's
// default chain only.
void prepareModules(proto::ProtoSpace& space) {
#if defined(PROTOCLJ_EXAMPLE_GEOMETRY)
    example::registerGeometryProvider();
    proto::ProtoContext* ctx = space.rootContext;
    const proto::ProtoList* chain = ctx->newList()->appendLast(
        ctx, ctx->fromUTF8String("provider:example_geometry"));
    const proto::ProtoObject* existing = space.getResolutionChain();
    if (existing && existing != PROTO_NONE && existing->asList(ctx)) {
        const proto::ProtoList* rest = existing->asList(ctx);
        for (proto::proto_ulong i = 0; i < rest->getSize(ctx); ++i)
            chain = chain->appendLast(ctx, rest->getAt(ctx, static_cast<int>(i)));
    }
    space.setResolutionChain(chain->asObject(ctx));
#else
    (void)space;
#endif
}

} // namespace

int main(int argc, char** argv) {
    prepareStandardStreams();
    // Every non-tail call nests the VM on the native stack, so the stack
    // size bounds the recursion depth. The evaluator runs on a thread with
    // a stack of protoClojure::kThreadStackBytes, and every thread created
    // later (futures, pmap, actor workers) gets the same size by default;
    // the VM raises StackOverflowError before any of them overflows.
    protoClojure::configureThreadStacks();

    if (argc < 2) {
        // No arguments → drop into the interactive REPL.
        proto::ProtoSpace space;
        applyHeapPolicy(space);
        prepareModules(space);
        return protoClojure::runOnEvaluatorThread(
            space, [](proto::ProtoContext* ctx, void*) { return protoClojure::runRepl(ctx); },
            nullptr);
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
        // Positional: a file to run. Every argument after it belongs to the
        // script (`*command-line-args*`), flags included.
        ScriptRun run{argv[i], {}};
        for (int j = i + 1; j < argc; ++j) run.args.emplace_back(argv[j]);
        proto::ProtoSpace space;
        applyHeapPolicy(space);
        prepareModules(space);
        return protoClojure::runOnEvaluatorThread(
            space,
            [](proto::ProtoContext* ctx, void* p) {
                auto* r = static_cast<ScriptRun*>(p);
                return runFile(ctx, r->path, r->args);
            },
            static_cast<void*>(&run));
    }
    return 0;
}
