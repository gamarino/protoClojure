/*
 * protoclj — command-line entry point.
 *
 * v0.0.x scope: print version and help. No reader, no compiler, no VM yet.
 * The flag surface mirrors what Phase 1 will fill out (--version, --help,
 * eventually -e and a file argument); landing the parser shape now means
 * subsequent sessions only add behaviour, not flag scaffolding.
 *
 * Every interpreter feature lands behind one of these flags. We deliberately
 * link protoCore from the first commit so a build break shows up immediately
 * rather than three sessions in.
 */
#include "protoClojure.h"
#include "protoCore.h"

#include <cstring>
#include <cstdio>
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
        "Options (v0.0.x — only the placeholder flags are wired):\n"
        "  --version       Print version and exit.\n"
        "  --help, -h      Print this help and exit.\n"
        "\n"
        "Not yet implemented (tracked in Phase 1 of docs/ROADMAP.md):\n"
        "  -e <expr>       Evaluate one expression (Phase 1 — Week 1).\n"
        "  script.clj      Run a file (Phase 1 — Week 1).\n"
        "  (no args)       Start the interactive REPL (Phase 1 — Week 4).\n"
        "  --nrepl PORT    Start the nREPL server (Phase 5).\n");
}

// A tiny protoCore liveness check: construct a ProtoSpace, fetch the root
// context, and confirm it is non-null. Costs ~ms at startup; catches "protoCore
// linked but broken" the moment anything is wrong with the embedding.
bool protoCoreHandshakeOk() {
    try {
        proto::ProtoSpace space;
        proto::ProtoContext* ctx = space.rootContext;
        return ctx != nullptr;
    } catch (...) {
        return false;
    }
}

} // namespace

int main(int argc, char** argv) {
    // v0.0.x: only --version and --help are wired. Everything else is a
    // friendly "not implemented yet, see ROADMAP.md".
    if (argc < 2) {
        // No args: would become REPL launch in Phase 1 Week 4. For now,
        // report the gap rather than silently exit.
        printVersion();
        std::printf(
            "(interactive REPL not yet implemented — Phase 1 Week 4)\n"
            "Run with --help to see the planned surface.\n");
        if (!protoCoreHandshakeOk()) {
            std::fprintf(stderr,
                "warning: protoCore handshake failed at startup\n");
            return 2;
        }
        return 0;
    }

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
        std::fprintf(stderr,
            "protoclj: '%s' is not yet implemented (see docs/ROADMAP.md). "
            "Try --help.\n", a);
        return 1;
    }
    return 0;
}
