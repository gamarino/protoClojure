#include "repl/Repl.h"

#include "protoClojure.h"
#include "protoCore.h"

#include "reader/Reader.h"
#include "compiler/Compiler.h"
#include "runtime/ActorScheduler.h"
#include "runtime/BytecodeModule.h"
#include "runtime/ExecutionEngine.h"
#include "runtime/Opcodes.h"
#include "runtime/Primitives.h"

#include <readline/history.h>
#include <readline/readline.h>
// readline/chardefs.h defines a `RETURN` macro (= CTRL('M')) which would
// turn our `Op::RETURN` into a syntax error. Undefine it here — we only
// care about the enum value.
#ifdef RETURN
#  undef RETURN
#endif

#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace protoClojure {

namespace {

// --- delimiter-balance scanner --------------------------------------------
//
// Decides whether an accumulated buffer is a complete top-level form or
// the user is still typing. Cheap, lexical only: tracks `()`, `[]`, `{}`,
// `"…"` strings, `;` line comments, and `\X` character literals. A
// negative paren / bracket / brace count → genuine error (we let the
// reader report it). String continuation → still typing.
enum class Balance { Incomplete, Balanced, Error };

Balance scanBalance(const std::string& src) {
    long paren = 0, bracket = 0, brace = 0;
    bool inStr = false;
    for (std::size_t i = 0; i < src.size(); ++i) {
        char c = src[i];
        if (inStr) {
            if (c == '\\' && i + 1 < src.size()) { ++i; continue; }
            if (c == '"') inStr = false;
            continue;
        }
        if (c == ';') {
            // Line comment — skip to newline (or EOF).
            while (i < src.size() && src[i] != '\n') ++i;
            continue;
        }
        switch (c) {
            case '"': inStr = true;       break;
            case '(': ++paren;            break;
            case ')': if (--paren < 0)   return Balance::Error;  break;
            case '[': ++bracket;          break;
            case ']': if (--bracket < 0) return Balance::Error;  break;
            case '{': ++brace;            break;
            case '}': if (--brace < 0)   return Balance::Error;  break;
            case '\\':
                // Character literal — Clojure's `\(`, `\)`, `\[` etc. are
                // legal. Skip one char so a literal close-paren doesn't
                // falsely close a form.
                if (i + 1 < src.size()) ++i;
                break;
            default: break;
        }
    }
    if (inStr)                                       return Balance::Incomplete;
    if (paren > 0 || bracket > 0 || brace > 0)       return Balance::Incomplete;
    return Balance::Balanced;
}

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])))     ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string historyPath() {
    const char* home = std::getenv("HOME");
    if (!home || !*home) return std::string();
    return std::string(home) + "/.protoclj_history";
}

// --- line input ------------------------------------------------------------

// Read one logical input line. Wraps libreadline on an interactive tty;
// falls back to std::getline-shaped `getc` on a piped stdin so the REPL
// is scriptable (`echo '(+ 1 2)' | protoclj`) and testable.
bool readLine(const char* prompt, bool interactive,
              std::string& out, bool* eof) {
    *eof = false;
    if (interactive) {
        char* line = ::readline(prompt);
        if (!line) { *eof = true; return false; }
        out.assign(line);
        std::free(line);
        return true;
    }
    std::fputs(prompt, stdout);
    std::fflush(stdout);
    std::string buf;
    int ch;
    bool any = false;
    while ((ch = std::getc(stdin)) != EOF) {
        any = true;
        if (ch == '\n') { out = buf; return true; }
        buf.push_back(static_cast<char>(ch));
    }
    if (any) { out = buf; return true; }
    *eof = true;
    return false;
}

// --- session state --------------------------------------------------------
//
// Everything the REPL needs to keep between forms: ProtoSpace + ctx, the
// markers (mirrors main.cpp::runFile slot layout), the interned attribute
// keys, the active CompilerMarkers / ReaderMarkers structs, and the keys
// for `*1` / `*2` / `*3` rebinding.
struct Session {
    proto::ProtoSpace space;
    proto::ProtoContext* ctx;

    proto::ProtoObject* globals;

    const proto::ProtoObject* stringMarker;
    const proto::ProtoObject* fnSingleProto;
    const proto::ProtoObject* fnMultiProto;
    const proto::ProtoObject* vectorMarker;
    const proto::ProtoObject* mapMarker;
    const proto::ProtoObject* atomMarker;
    const proto::ProtoObject* futureMarker;
    const proto::ProtoObject* promiseMarker;
    const proto::ProtoObject* actorMarker;

    const proto::ProtoString* bytesKey;
    const proto::ProtoString* bytecodeKey;
    const proto::ProtoString* arityKey;
    const proto::ProtoString* capturesKey;
    const proto::ProtoString* aritiesKey;
    const proto::ProtoString* itemsKey;
    const proto::ProtoString* entriesKey;
    const proto::ProtoString* valueKey;
    const proto::ProtoString* watchesKey;
    const proto::ProtoString* thunkKey;
    const proto::ProtoString* ccBlobKey;
    const proto::ProtoString* threadKey;
    const proto::ProtoString* resultKey;
    const proto::ProtoString* doneKey;
    const proto::ProtoString* actorStateKey;

    const proto::ProtoString* star1Key;
    const proto::ProtoString* star2Key;
    const proto::ProtoString* star3Key;

    ReaderMarkers      readerMarkers;
    CompilerMarkers    compilerMarkers;
    ActiveCallContext  printerCc;     // installed on TLS for replPrintValue
                                      // so the atom/future/promise/map/actor
                                      // branches in the printer can see the
                                      // markers.  Engine swaps this for its
                                      // own during `run()` and restores it.
    ExecutionEngine    printerEngine; // a dedicated engine handle is fine —
                                      // the printer never invokes it; it's
                                      // just a non-null pointer to slot into
                                      // ActiveCallContext::engine.

    // BytecodeModules accumulated across the session. `defn` (and the
    // anonymous-fn forms) emit instructions into the per-form module and
    // store the byte buffer's address on the resulting fn wrapper as
    // `__bytecode__`. If the module dies the wrapper's bytecode pointer
    // dangles — calling `(my-fn)` later then dereferences freed memory
    // (SIGSEGV reproduced on the first piped REPL session: defn worked,
    // a call to the just-defined name crashed). Retaining every module
    // for the whole REPL session is the same fix protoST's REPL uses.
    std::vector<std::unique_ptr<BytecodeModule>> retainedModules;

    Session()
        : ctx(space.rootContext) {
        ctx->resizeAutomaticLocals(11);

        globals = const_cast<proto::ProtoObject*>(
            space.objectPrototype->newChild(ctx, /*isMutable=*/true));
        ctx->setAutomaticLocal(0, globals);

        protoClojure::installPrimitives(ctx, globals);

        stringMarker   = space.objectPrototype->newChild(ctx, true);
        fnSingleProto  = space.objectPrototype->newChild(ctx, true);
        fnMultiProto   = space.objectPrototype->newChild(ctx, true);
        vectorMarker   = space.objectPrototype->newChild(ctx, true);
        mapMarker      = space.objectPrototype->newChild(ctx, true);
        atomMarker     = space.objectPrototype->newChild(ctx, true);
        futureMarker   = space.objectPrototype->newChild(ctx, true);
        promiseMarker  = space.objectPrototype->newChild(ctx, true);
        actorMarker    = space.objectPrototype->newChild(ctx, true);

        // Pin the markers in slots so the GC sees them; main.cpp does
        // the same per session-3 engineering principles (P1 / P2).
        ctx->setAutomaticLocal(2,  stringMarker);
        ctx->setAutomaticLocal(3,  fnSingleProto);
        ctx->setAutomaticLocal(4,  vectorMarker);
        ctx->setAutomaticLocal(5,  fnMultiProto);
        ctx->setAutomaticLocal(6,  mapMarker);
        ctx->setAutomaticLocal(7,  atomMarker);
        ctx->setAutomaticLocal(8,  futureMarker);
        ctx->setAutomaticLocal(9,  promiseMarker);
        ctx->setAutomaticLocal(10, actorMarker);

        bytesKey      = proto::ProtoString::createSymbol(ctx, "__bytes__");
        bytecodeKey   = proto::ProtoString::createSymbol(ctx, "__bytecode__");
        arityKey      = proto::ProtoString::createSymbol(ctx, "__arity__");
        capturesKey   = proto::ProtoString::createSymbol(ctx, "__captures__");
        aritiesKey    = proto::ProtoString::createSymbol(ctx, "__arities__");
        itemsKey      = proto::ProtoString::createSymbol(ctx, "__items__");
        entriesKey    = proto::ProtoString::createSymbol(ctx, "__entries__");
        valueKey      = proto::ProtoString::createSymbol(ctx, "__value__");
        watchesKey    = proto::ProtoString::createSymbol(ctx, "__watches__");
        thunkKey      = proto::ProtoString::createSymbol(ctx, "__thunk__");
        ccBlobKey     = proto::ProtoString::createSymbol(ctx, "__cc_blob__");
        threadKey     = proto::ProtoString::createSymbol(ctx, "__thread__");
        resultKey     = proto::ProtoString::createSymbol(ctx, "__result__");
        doneKey       = proto::ProtoString::createSymbol(ctx, "__done__");
        actorStateKey = proto::ProtoString::createSymbol(ctx, "__actor_state__");

        star1Key      = proto::ProtoString::createSymbol(ctx, "*1");
        star2Key      = proto::ProtoString::createSymbol(ctx, "*2");
        star3Key      = proto::ProtoString::createSymbol(ctx, "*3");

        // Seed *1 *2 *3 with nil so they read cleanly before any eval.
        globals->setAttribute(ctx, star1Key, PROTO_NONE);
        globals->setAttribute(ctx, star2Key, PROTO_NONE);
        globals->setAttribute(ctx, star3Key, PROTO_NONE);

        readerMarkers   = ReaderMarkers{stringMarker, vectorMarker, mapMarker,
                                        bytesKey, itemsKey, entriesKey};
        compilerMarkers = CompilerMarkers{stringMarker, vectorMarker, mapMarker,
                                          bytesKey, bytecodeKey, arityKey,
                                          capturesKey, aritiesKey,
                                          itemsKey, entriesKey};

        // Build the ActiveCallContext the printer needs. ExecutionEngine::run
        // installs its own cc for the duration of a call (then restores
        // whatever was there); installing this one once at session-start
        // means the cc is always non-null when replPrintValue runs, so the
        // atom / future / promise / map / actor branches in the printer
        // recognise their markers and emit `#<atom 0>` / `#<future …>`
        // instead of `#<unprintable>`.
        printerCc = ActiveCallContext{
            &printerEngine, globals,
            fnSingleProto, fnMultiProto, mapMarker, atomMarker,
            futureMarker, promiseMarker, actorMarker,
            bytecodeKey, arityKey, capturesKey, aritiesKey,
            entriesKey, valueKey, watchesKey,
            thunkKey, ccBlobKey, threadKey, resultKey, doneKey,
            actorStateKey};
        setActiveCallContext(printerCc);
    }

    // Run one accumulated form. Returns true on a clean eval; the result
    // value is placed in `*out` on success (nullptr on error). Errors are
    // printed to stderr and the REPL loop continues.
    bool evalForm(const std::string& src, const proto::ProtoObject** out) {
        *out = nullptr;
        Reader reader(ctx, src, readerMarkers);
        const proto::ProtoObject* form = nullptr;
        try {
            form = reader.readOne();
        } catch (const ReaderError& e) {
            std::fprintf(stderr, "read error: %s\n", e.what());
            return false;
        }
        if (!form) return true;  // empty input — treat as a no-op

        auto modPtr = std::make_unique<BytecodeModule>();
        BytecodeModule& mod = *modPtr;
        Compiler compiler;
        try {
            compiler.compileForm(ctx, form, mod, compilerMarkers);
        } catch (const CompileError& e) {
            std::fprintf(stderr, "compile error: %s\n", e.what());
            return false;
        }
        mod.emit(Op::RETURN, 0);
        // Retain BEFORE run — the fn objects defn allocates carry a
        // pointer into mod's byte buffer, and they have to survive past
        // this scope.
        retainedModules.push_back(std::move(modPtr));

        // Use a separate ExecutionEngine instance per form. Cheap; keeps
        // the engine's internal state from leaking across forms.
        ExecutionEngine eng;
        const proto::ProtoObject* result = nullptr;
        try {
            result = eng.run(ctx, mod, globals,
                fnSingleProto, fnMultiProto, mapMarker, atomMarker,
                futureMarker, promiseMarker, actorMarker,
                bytecodeKey, arityKey, capturesKey, aritiesKey,
                entriesKey, valueKey, watchesKey,
                thunkKey, ccBlobKey, threadKey, resultKey, doneKey,
                actorStateKey);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "error: %s\n", e.what());
            return false;
        }
        *out = result;
        return true;
    }

    // After each successful eval, shift the *1 / *2 / *3 chain so the
    // newest result is `*1`, the previous one drops to `*2`, etc.
    void bindStars(const proto::ProtoObject* fresh) {
        const proto::ProtoObject* prev1 =
            globals->getAttribute(ctx, star1Key);
        const proto::ProtoObject* prev2 =
            globals->getAttribute(ctx, star2Key);
        globals->setAttribute(ctx, star3Key, prev2 ? prev2 : PROTO_NONE);
        globals->setAttribute(ctx, star2Key, prev1 ? prev1 : PROTO_NONE);
        globals->setAttribute(ctx, star1Key, fresh ? fresh : PROTO_NONE);
    }
};

// --- meta-commands --------------------------------------------------------

void printHelp() {
    std::puts("REPL commands (only at the primary prompt):");
    std::puts("  :help, :h         Show this message");
    std::puts("  :quit, :q         Exit the REPL (also Ctrl-D)");
    std::puts("  :load <path>      Read and evaluate a .clj file in this session");
    std::puts("  :time <expr>      Evaluate <expr>, report wall-clock time");
    std::puts("Bindings: *1, *2, *3 hold the last three evaluation results.");
    std::puts("Multi-line: open a `(`, `[`, or `{` and keep typing — the prompt");
    std::puts("becomes `  #_=>` until the form closes. Blank line at the");
    std::puts("continuation prompt forces evaluation.");
}

void cmdLoad(Session& s, const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, ":load: cannot open %s\n", path.c_str());
        return;
    }
    std::stringstream buf; buf << f.rdbuf();
    std::string src = buf.str();

    // :load runs every form in the file like the original `protoclj
    // <file>` path, but against the live session — defs persist.
    Reader reader(s.ctx, src, s.readerMarkers);
    const proto::ProtoList* forms = nullptr;
    try {
        forms = reader.readAll();
    } catch (const ReaderError& e) {
        std::fprintf(stderr, "%s:%d:%d: read error: %s\n",
                     path.c_str(), e.line, e.column, e.what());
        return;
    }
    unsigned long n = forms->getSize(s.ctx);
    for (unsigned long i = 0; i < n; ++i) {
        const proto::ProtoObject* form =
            forms->getAt(s.ctx, static_cast<int>(i));
        auto modPtr = std::make_unique<BytecodeModule>();
        BytecodeModule& mod = *modPtr;
        Compiler compiler;
        try {
            compiler.compileForm(s.ctx, form, mod, s.compilerMarkers);
        } catch (const CompileError& e) {
            std::fprintf(stderr, "%s: compile error: %s\n",
                         path.c_str(), e.what());
            return;
        }
        mod.emit(Op::RETURN, 0);
        s.retainedModules.push_back(std::move(modPtr));
        ExecutionEngine eng;
        try {
            eng.run(s.ctx, mod, s.globals,
                s.fnSingleProto, s.fnMultiProto, s.mapMarker, s.atomMarker,
                s.futureMarker, s.promiseMarker, s.actorMarker,
                s.bytecodeKey, s.arityKey, s.capturesKey, s.aritiesKey,
                s.entriesKey, s.valueKey, s.watchesKey,
                s.thunkKey, s.ccBlobKey, s.threadKey, s.resultKey, s.doneKey,
                s.actorStateKey);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "%s: runtime error: %s\n",
                         path.c_str(), e.what());
            return;
        }
    }
    std::printf("loaded %s (%lu forms)\n", path.c_str(), n);
}

void cmdTime(Session& s, const std::string& expr) {
    auto t0 = std::chrono::steady_clock::now();
    const proto::ProtoObject* r = nullptr;
    bool ok = s.evalForm(expr, &r);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    if (ok) {
        replPrintValue(s.ctx, stdout, r);
        std::fputc('\n', stdout);
        s.bindStars(r);
    }
    std::printf("elapsed: %.3f ms\n", ms);
}

// Dispatch a `:`-prefixed line. Returns true if the loop should exit.
bool dispatchMeta(Session& s, const std::string& trimmed) {
    std::size_t sp = trimmed.find_first_of(" \t");
    std::string cmd = (sp == std::string::npos) ? trimmed
                                                : trimmed.substr(0, sp);
    std::string arg = (sp == std::string::npos) ? std::string()
                                                : trim(trimmed.substr(sp + 1));
    if (cmd == ":quit" || cmd == ":q") {
        std::puts("Bye for now.");
        return true;
    }
    if (cmd == ":help" || cmd == ":h") { printHelp(); return false; }
    if (cmd == ":load") {
        if (arg.empty()) {
            std::fprintf(stderr, ":load requires a file path\n");
        } else {
            cmdLoad(s, arg);
        }
        return false;
    }
    if (cmd == ":time") {
        if (arg.empty()) {
            std::fprintf(stderr, ":time requires an expression\n");
        } else {
            cmdTime(s, arg);
        }
        return false;
    }
    std::fprintf(stderr, "unknown command: %s (try :help)\n", cmd.c_str());
    return false;
}

} // namespace

int runRepl() {
    const bool interactive = ::isatty(STDIN_FILENO) != 0;

    std::string histPath = historyPath();
    if (interactive && !histPath.empty()) {
        ::read_history(histPath.c_str());
    }

    std::printf("protoClojure %s\n", versionString());
    std::puts("REPL — :help for commands, :quit or Ctrl-D to exit");

    Session session;

    const char* primary      = "user=> ";
    const char* continuation = "  #_=> ";

    std::string buffer;
    bool inMultiline = false;

    for (;;) {
        std::string line;
        bool eof = false;
        const char* prompt = inMultiline ? continuation : primary;
        if (!readLine(prompt, interactive, line, &eof)) {
            if (eof) {
                std::puts(interactive ? "\nBye for now." : "Bye for now.");
                break;
            }
            continue;
        }

        std::string trimmed = trim(line);

        if (!inMultiline && !trimmed.empty() && trimmed[0] == ':') {
            if (interactive) ::add_history(line.c_str());
            if (dispatchMeta(session, trimmed)) break;
            continue;
        }

        if (interactive && !trimmed.empty()) {
            ::add_history(line.c_str());
        }

        const bool blank = trimmed.empty();
        if (!inMultiline) {
            if (blank) continue;
            buffer = line;
        } else {
            buffer += "\n";
            buffer += line;
        }

        // Force-flush a stuck multi-line form.
        if (inMultiline && blank) {
            const proto::ProtoObject* r = nullptr;
            if (session.evalForm(buffer, &r) && r) {
                replPrintValue(session.ctx, stdout, r);
                std::fputc('\n', stdout);
                session.bindStars(r);
            }
            buffer.clear();
            inMultiline = false;
            std::fflush(stdout);
            continue;
        }

        Balance b = scanBalance(buffer);
        if (b == Balance::Incomplete) {
            inMultiline = true;
            continue;
        }
        // Balanced (or Error — let the reader complain). Eval.
        const proto::ProtoObject* r = nullptr;
        bool ok = session.evalForm(buffer, &r);
        if (ok && r) {
            replPrintValue(session.ctx, stdout, r);
            std::fputc('\n', stdout);
            session.bindStars(r);
        }
        buffer.clear();
        inMultiline = false;
        std::fflush(stdout);
    }

    if (interactive && !histPath.empty()) {
        ::write_history(histPath.c_str());
    }

    // Same shutdown sequence as main.cpp::runFile() — join future and
    // actor worker threads before the ProtoSpace destructor runs.
    shutdownFutures(session.ctx);
    ActorScheduler::instance().shutdown(session.ctx);
    return 0;
}

} // namespace protoClojure
