/*
 * protoclj — command-line entry point.
 *
 * v0.0.x scope: --version, --help, and *running a .clj file* through the
 * reader/compiler/VM pipeline. Subsequent sessions add -e (one-form eval),
 * the interactive REPL (session 4-ish), and --nrepl (phase 5).
 */
#include "protoClojure.h"
#include "protoCore.h"

#include "reader/Reader.h"
#include "compiler/Compiler.h"
#include "runtime/BytecodeModule.h"
#include "runtime/ExecutionEngine.h"
#include "runtime/Primitives.h"

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
        "Not yet implemented:\n"
        "  -e <expr>       Evaluate one expression (Phase 1 — Week 1+).\n"
        "  (no args)       Start the interactive REPL (Phase 1 — Week 4).\n"
        "  --nrepl PORT    Start the nREPL server (Phase 5).\n");
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

    // Slots layout (kept stable for the whole run, rooted via the root ctx):
    //   0 : globals namespace (mutable child of objectPrototype).
    //   1 : forms (the ProtoList readAll() returned).
    //   2 : stringMarkerProto — see ReaderMarkers / CompilerMarkers docs.
    //   3 : fnMarkerProto — wraps user-fn callables (session 5).
    ctx->resizeAutomaticLocals(4);
    constexpr unsigned int kSlotGlobals      = 0;
    constexpr unsigned int kSlotForms        = 1;
    constexpr unsigned int kSlotStringMarker = 2;
    constexpr unsigned int kSlotFnMarker     = 3;

    const proto::ProtoObject* globalsObj =
        space.objectPrototype->newChild(ctx, /*isMutable=*/true);
    ctx->setAutomaticLocal(kSlotGlobals, globalsObj);

    protoClojure::installPrimitives(
        ctx, const_cast<proto::ProtoObject*>(
            ctx->getAutomaticLocal(kSlotGlobals)));

    const proto::ProtoObject* stringMarkerProto =
        space.objectPrototype->newChild(ctx, /*isMutable=*/true);
    ctx->setAutomaticLocal(kSlotStringMarker, stringMarkerProto);

    const proto::ProtoObject* fnMarkerProto =
        space.objectPrototype->newChild(ctx, /*isMutable=*/true);
    ctx->setAutomaticLocal(kSlotFnMarker, fnMarkerProto);

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

    protoClojure::ReaderMarkers readerMarkers{
        ctx->getAutomaticLocal(kSlotStringMarker), bytesKey};
    protoClojure::CompilerMarkers compilerMarkers{
        ctx->getAutomaticLocal(kSlotStringMarker),
        ctx->getAutomaticLocal(kSlotFnMarker),
        bytesKey, bytecodeKey, arityKey, capturesKey, aritiesKey};

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
        } catch (const protoClojure::CompileError& e) {
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
                ctx->getAutomaticLocal(kSlotFnMarker),
                bytecodeKey, arityKey, capturesKey, aritiesKey);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s: runtime error: %s\n", path, e.what());
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printVersion();
        std::printf(
            "(interactive REPL not yet implemented — Phase 1 Week 4)\n"
            "Run with --help to see the planned surface.\n");
        return 0;
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
        return runFile(a);
    }
    return 0;
}
