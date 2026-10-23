#include "repl/Repl.h"

#include "protoClojure.h"
#include "protoCore.h"

#include "reader/Reader.h"
#include "compiler/Compiler.h"
#include "compiler/Eval.h"
#include "runtime/ActorScheduler.h"
#include "runtime/BytecodeModule.h"
#include "runtime/Exceptions.h"
#include "runtime/ExecutionEngine.h"
#include "runtime/IO.h"
#include "runtime/Named.h"
#include "runtime/Opcodes.h"
#include "runtime/Primitives.h"

#if defined(PROTOCLJ_NO_READLINE)
// No libreadline (Windows). The console edits the line and keeps a history of
// its own, so these stand-ins read a plain line and keep no history file.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <io.h>
#include <string>
#define isatty _isatty
#define STDIN_FILENO 0
static char* readline(const char* prompt) {
    std::fputs(prompt, stdout);
    std::fflush(stdout);
    std::string line;
    int ch;
    bool any = false;
    while ((ch = std::getc(stdin)) != EOF && ch != '\n') { any = true; line.push_back(static_cast<char>(ch)); }
    if (ch == EOF && !any) return nullptr;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    char* out = static_cast<char*>(std::malloc(line.size() + 1));
    std::memcpy(out, line.c_str(), line.size() + 1);
    return out;
}
static void add_history(const char*) {}
static int read_history(const char*) { return 0; }
static int write_history(const char*) { return 0; }
#else
#include <readline/history.h>
#include <readline/readline.h>
// readline/chardefs.h defines a `RETURN` macro (= CTRL('M')) which would
// turn our `Op::RETURN` into a syntax error. Undefine it here — we only
// care about the enum value.
#ifdef RETURN
#  undef RETURN
#endif
#include <unistd.h>
#endif

#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace protoClojure {

namespace {

// --- delimiter-balance scanner --------------------------------------------
//
// Decides whether an accumulated buffer is a complete top-level form or
// the user is still typing. Cheap, lexical only: tracks `()`, `[]`, `{}`,
// `"…"` strings, `;` and `#!` line comments, and `\X` character literals. A
// negative paren / bracket / brace count → genuine error (we let the
// reader report it). String continuation → still typing.
enum class Balance { Incomplete, Balanced, Error };

// True when `c` ends a token, so a following `'` starts a quote rather
// than continuing a symbol (`foo'` is a symbol).
bool endsBeforeMacro(char c) {
    return std::isspace(static_cast<unsigned char>(c)) || c == ',' || c == '(' ||
           c == '[' || c == '{' || c == '\'' || c == '`' || c == '~' || c == '@';
}

Balance scanBalance(const std::string& src) {
    long paren = 0, bracket = 0, brace = 0;
    bool inStr = false;
    // A reader macro (`'`, `` ` ``, `~`, `~@`, `@`) as the last thing typed
    // still needs its form: the input is incomplete, as in Clojure's REPL.
    bool pendingMacro = false;
    for (std::size_t i = 0; i < src.size(); ++i) {
        char c = src[i];
        if (!inStr && !std::isspace(static_cast<unsigned char>(c)) && c != ',' && c != ';' &&
            !(c == '#' && i + 1 < src.size() && src[i + 1] == '!')) {
            pendingMacro = (c == '`' || c == '~' || c == '@' ||
                            (c == '\'' && (i == 0 || endsBeforeMacro(src[i - 1]))));
        }
        if (inStr) {
            if (c == '\\' && i + 1 < src.size()) { ++i; continue; }
            if (c == '"') inStr = false;
            continue;
        }
        if (c == ';' || (c == '#' && i + 1 < src.size() && src[i + 1] == '!')) {
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
    if (pendingMacro)                                return Balance::Incomplete;
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
    proto::ProtoSpace& space;
    proto::ProtoContext* ctx;

    proto::ProtoObject* globals;

    const proto::ProtoObject* fnSingleProto;
    const proto::ProtoObject* fnMultiProto;
    const proto::ProtoObject* atomMarker;
    const proto::ProtoObject* futureMarker;
    const proto::ProtoObject* promiseMarker;
    const proto::ProtoObject* actorMarker;
    NamedLayout               named;          // keywords and symbols (Named.h)

    const proto::ProtoString* bytecodeKey;
    const proto::ProtoString* arityKey;
    const proto::ProtoString* capturesKey;
    const proto::ProtoString* aritiesKey;
    const proto::ProtoString* valueKey;
    const proto::ProtoString* watchesKey;
    const proto::ProtoString* thunkKey;
    const proto::ProtoString* ccBlobKey;
    const proto::ProtoString* threadKey;
    const proto::ProtoString* resultKey;
    const proto::ProtoString* doneKey;
    const proto::ProtoString* actorStateKey;
    const proto::ProtoString* mailboxKey;

    const proto::ProtoString* star1Key;
    const proto::ProtoString* star2Key;
    const proto::ProtoString* star3Key;

    ActiveCallContext  sessionCc;     // installed on TLS for the whole
                                      // session: evalTopLevel runs forms on
                                      // it, macros run on it while a form is
                                      // compiled, and the printer reads its
                                      // markers. `run()` installs an equal
                                      // one and restores this one.
    ExecutionEngine    engine;        // stateless; the engine every form,
                                      // and every macro, runs on

    explicit Session(proto::ProtoContext* c)
        : space(*c->space), ctx(c) {
        ctx->resizeAutomaticLocals(10);

        globals = const_cast<proto::ProtoObject*>(
            space.objectPrototype->newChild(ctx, /*isMutable=*/true));
        ctx->setAutomaticLocal(0, globals);

        protoClojure::installPrimitives(ctx, globals);

        // Prototype markers are never mutated, so they are immutable
        // objects; only the globals and the Named table are mutable.
        fnSingleProto  = space.objectPrototype->newChild(ctx);
        fnMultiProto   = space.objectPrototype->newChild(ctx);
        atomMarker     = space.objectPrototype->newChild(ctx);
        futureMarker   = space.objectPrototype->newChild(ctx);
        promiseMarker  = space.objectPrototype->newChild(ctx);
        actorMarker    = space.objectPrototype->newChild(ctx);

        // Pin the markers in slots so the GC sees them; main.cpp does
        // the same per session-3 engineering principles (P1 / P2).
        ctx->setAutomaticLocal(1,  fnSingleProto);
        ctx->setAutomaticLocal(2,  fnMultiProto);
        ctx->setAutomaticLocal(3,  atomMarker);
        ctx->setAutomaticLocal(4,  futureMarker);
        ctx->setAutomaticLocal(5,  promiseMarker);
        ctx->setAutomaticLocal(6,  actorMarker);

        // Keywords and symbols: the prototype of every named value and the
        // table that interns them by spelling, pinned like the markers.
        named.marker = space.objectPrototype->newChild(ctx);
        ctx->setAutomaticLocal(7, named.marker);
        named.table = space.objectPrototype->newChild(ctx, true);
        ctx->setAutomaticLocal(8, named.table);

        named.spellingKey = proto::ProtoString::createSymbol(ctx, "__spelling__");

        bytecodeKey   = proto::ProtoString::createSymbol(ctx, "__bytecode__");
        arityKey      = proto::ProtoString::createSymbol(ctx, "__arity__");
        capturesKey   = proto::ProtoString::createSymbol(ctx, "__captures__");
        aritiesKey    = proto::ProtoString::createSymbol(ctx, "__arities__");
        valueKey      = proto::ProtoString::createSymbol(ctx, "__value__");
        watchesKey    = proto::ProtoString::createSymbol(ctx, "__watches__");
        thunkKey      = proto::ProtoString::createSymbol(ctx, "__thunk__");
        ccBlobKey     = proto::ProtoString::createSymbol(ctx, "__cc_blob__");
        threadKey     = proto::ProtoString::createSymbol(ctx, "__thread__");
        resultKey     = proto::ProtoString::createSymbol(ctx, "__result__");
        doneKey       = proto::ProtoString::createSymbol(ctx, "__done__");
        actorStateKey = proto::ProtoString::createSymbol(ctx, "__actor_state__");
        mailboxKey    = proto::ProtoString::createSymbol(ctx, "__mailbox__");

        star1Key      = proto::ProtoString::createSymbol(ctx, "*1");
        star2Key      = proto::ProtoString::createSymbol(ctx, "*2");
        star3Key      = proto::ProtoString::createSymbol(ctx, "*3");

        // Seed *1 *2 *3 with nil so they read cleanly before any eval.
        globals->setAttribute(ctx, star1Key, PROTO_NONE);
        globals->setAttribute(ctx, star2Key, PROTO_NONE);
        globals->setAttribute(ctx, star3Key, PROTO_NONE);

        // Build the ActiveCallContext the printer needs. ExecutionEngine::run
        // installs its own cc for the duration of a call (then restores
        // whatever was there); installing this one once at session-start
        // means the cc is always non-null when replPrintValue runs, so the
        // atom / future / promise / map / actor branches in the printer
        // recognise their markers and emit `#<atom 0>` / `#<future …>`
        // instead of `#<unprintable>`.
        sessionCc = ActiveCallContext{
            &engine, globals,
            fnSingleProto, fnMultiProto, atomMarker,
            futureMarker, promiseMarker, actorMarker,
            bytecodeKey, arityKey, capturesKey, aritiesKey,
            valueKey, watchesKey,
            thunkKey, ccBlobKey, threadKey, resultKey, doneKey,
            actorStateKey, mailboxKey, named};
        setActiveCallContext(sessionCc);
        installEval();
    }

    // Read, compile and run every form in `src`, one at a time, as Clojure's
    // REPL does: a form is compiled only after the one before it has run,
    // so `(defmacro m ...) (m)` on one line works. Each result is handed to
    // `onResult` while it is still rooted (the caller prints it and binds
    // *1). A compile or runtime error is printed and the REPL goes on with
    // the next form of the input, as Clojure's REPL does; a read error drops
    // the rest of the input, the reader's position in it being unreliable.
    // Returns false when any form failed.
    //
    // Each form lives in a context of its own, rooted in its slot while it
    // is compiled and run, and is garbage afterwards unless something it
    // built is reachable from the globals (or *1 *2 *3).
    //
    // With `path` (`:load`), errors are reported as a script's are:
    // `path:line:col: read error`, `path:line:col: compile error` (where the
    // form starts) and `path: runtime error`, and the first error stops the
    // load, as it stops a script.
    bool evalSource(const std::string& src,
                    const std::function<void(const proto::ProtoObject*)>& onResult,
                    const std::string* path = nullptr) {
        Reader reader(ctx, src, named);
        reader.setSymbolResolver(&qualifySyntaxQuoteSymbol);
        bool ok = true;
        for (;;) {
            proto::ProtoContext formScope(ctx->space, ctx);
            formScope.resizeAutomaticLocals(2);  // the form, its result
            SourceInfo sourceInfo;
            reader.setSourceInfo(&sourceInfo);
            const proto::ProtoObject* form = nullptr;
            try {
                form = reader.readOne(&formScope);
            } catch (const ReaderError& e) {
                if (path) std::fprintf(stderr, "%s:%d:%d: read error: %s\n",
                                       path->c_str(), e.line, e.column, e.what());
                else      std::fprintf(stderr, "read error: %s\n", e.what());
                return false;
            }
            reader.setSourceInfo(nullptr);
            if (!form) return ok;  // end of the input
            formScope.setAutomaticLocal(0, form);

            // Expanded, compiled into a module of its own and run (Eval.h).
            try {
                formScope.setAutomaticLocal(1, evalTopLevel(&formScope,
                    formScope.getAutomaticLocal(0), &sourceInfo));
            } catch (const CompileError& e) {
                std::fflush(stdout);  // results printed so far come first
                if (path) std::fprintf(stderr, "%s:%d:%d: compile error: %s\n",
                                       path->c_str(), reader.formLine(),
                                       reader.formColumn(), e.what());
                else      std::fprintf(stderr, "compile error: %s\n", e.what());
                if (path) return false;
                ok = false;
                continue;
            } catch (const proto::ProtoException& e) {
                // An uncaught Clojure exception, protoCore error or module error.
                reportRuntimeError(path, describeThrown(&formScope, e.value()));
                if (path) return false;
                ok = false;
                continue;
            } catch (const std::exception& e) {
                reportRuntimeError(path, e.what());
                if (path) return false;
                ok = false;
                continue;
            } catch (...) {
                reportRuntimeError(path, "unknown C++ exception");
                if (path) return false;
                ok = false;
                continue;
            }
            onResult(formScope.getAutomaticLocal(1));
        }
    }

    static void reportRuntimeError(const std::string* path, const std::string& what) {
        std::fflush(stdout);  // results printed so far come first
        if (path) std::fprintf(stderr, "%s: runtime error: %s\n", path->c_str(), what.c_str());
        else      std::fprintf(stderr, "error: %s\n", what.c_str());
    }

    // Prints a result readably and makes it *1.
    void printAndBind(const proto::ProtoObject* r) {
        replPrintValue(ctx, stdout, r);
        std::fputc('\n', stdout);
        bindStars(r);
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

    // :load runs the file's forms one at a time, as `protoclj <file>` does,
    // but against the live session — defs persist. It stops at the first
    // error; the forms before it have run.
    proto::proto_ulong n = 0;
    if (s.evalSource(src, [&](const proto::ProtoObject*) { ++n; }, &path)) {
        std::printf("loaded %s (%" PROTO_FMT_U " forms)\n", path.c_str(), n);
    }
}

void cmdTime(Session& s, const std::string& expr) {
    // Times the reading, compiling and running of the forms, not the
    // printing of their results.
    double ms = 0.0;
    auto t0 = std::chrono::steady_clock::now();
    s.evalSource(expr, [&](const proto::ProtoObject* r) {
        ms += std::chrono::duration<double, std::milli>(
                  std::chrono::steady_clock::now() - t0).count();
        s.printAndBind(r);
        t0 = std::chrono::steady_clock::now();
    });
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

int runRepl(proto::ProtoContext* ctx) {
    const bool interactive = ::isatty(STDIN_FILENO) != 0;

    std::string histPath = historyPath();
    if (interactive && !histPath.empty()) {
        ::read_history(histPath.c_str());
    }

    std::printf("protoClojure %s\n", versionString());
    std::puts("REPL — :help for commands, :quit or Ctrl-D to exit");

    Session session(ctx);

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
            session.evalSource(buffer, [&](const proto::ProtoObject* r) {
                session.printAndBind(r);
            });
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
        session.evalSource(buffer, [&](const proto::ProtoObject* r) {
            session.printAndBind(r);
        });
        buffer.clear();
        inMultiline = false;
        std::fflush(stdout);
    }

    if (interactive && !histPath.empty()) {
        ::write_history(histPath.c_str());
    }

    // Same shutdown sequence as main.cpp::runFile() — stop HTTP servers,
    // then join future and actor worker threads before the ProtoSpace
    // destructor runs.
    shutdownIO(session.ctx);
    shutdownFutures(session.ctx);
    ActorScheduler::instance().shutdown(session.ctx);
    releaseRetainedModules();
    return 0;
}

} // namespace protoClojure
