#include "dap/DapServer.h"
#include "dap/DapStdio.h"
#include "dap/DapTransport.h"

#include "compiler/Compiler.h"
#include "compiler/Eval.h"
#include "reader/Lexer.h"
#include "reader/Reader.h"
#include "runtime/BytecodeModule.h"
#include "runtime/DebugHook.h"
#include "runtime/Exceptions.h"
#include "runtime/ExecutionEngine.h"
#include "runtime/LazySeq.h"
#include "runtime/MapOps.h"
#include "runtime/Primitives.h"
#include "runtime/SeqTypes.h"
#include "runtime/SetOps.h"
#include "runtime/VectorOps.h"

#include "protoCore.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace protoClojure {

namespace {

using nlohmann::json;

// ---------------------------------------------------------------------------
// Paths and breakpoint lines
// ---------------------------------------------------------------------------

// The form a path is compared in: VS Code sends `c:\Users\...` where the
// launch configuration may say `C:/Users/...`. Windows file names are not
// case-sensitive; elsewhere the path is compared as given.
std::string pathKey(std::string p) {
#if defined(_WIN32)
    for (char& c : p) {
        if (c == '\\') c = '/';
        else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
#endif
    return p;
}

std::string baseName(const std::string& path) {
    const std::size_t at = path.find_last_of("/\\");
    return at == std::string::npos ? path : path.substr(at + 1);
}

// ---------------------------------------------------------------------------
// pathMappings: the client's paths ("local") and the adapter's ("remote")
// ---------------------------------------------------------------------------

struct PathMapping {
    std::string local;    // the client's side: c:\Users\me\proj, /home/me/proj
    std::string remote;   // where the adapter runs
};

// A Windows-style path: a drive letter, a backslash, or a UNC prefix.
bool windowsLike(const std::string& p) {
    return (p.size() >= 2 && p[1] == ':' && std::isalpha(static_cast<unsigned char>(p[0]))) ||
           p.find('\\') != std::string::npos;
}

// `p` with `/` separators, lowercased when `fold`. Same length as `p`.
std::string normalizedPath(std::string p, bool fold) {
    for (char& c : p) {
        if (c == '\\') c = '/';
        else if (fold && c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return p;
}

// A root without its trailing separators (but "/" and "c:/" keep theirs).
std::string rootForm(std::string r) {
    while (r.size() > 1 && (r.back() == '/' || r.back() == '\\')) {
        if (r.size() == 3 && r[1] == ':') break;
        r.pop_back();
    }
    return r;
}

// Whether `p` is `root` or under it; `rest` is what follows the root, without
// its leading separator, spelled as in `p`.
bool underRoot(const std::string& p, const std::string& rootIn, bool fold, std::string& rest) {
    const std::string root = rootForm(rootIn);
    const std::string np = normalizedPath(p, fold), nr = normalizedPath(root, fold);
    if (nr.empty() || np.size() < nr.size() || np.compare(0, nr.size(), nr) != 0) return false;
    if (np.size() == nr.size()) { rest.clear(); return true; }
    if (nr.back() != '/' && np[nr.size()] != '/') return false;   // /proj vs /project
    std::size_t at = nr.size();
    while (at < p.size() && (p[at] == '/' || p[at] == '\\')) ++at;
    rest = p.substr(at);
    return true;
}

// `rest` (any separators) under `root`, in the root's own style.
std::string joinUnder(const std::string& rootIn, std::string rest) {
    const std::string root = rootForm(rootIn);
    const char sep = windowsLike(root) ? '\\' : '/';
    for (char& c : rest) if (c == '/' || c == '\\') c = sep;
    if (rest.empty()) return root;
    if (root.back() == '/' || root.back() == '\\') return root + rest;
    return root + sep + rest;
}

// The longest-prefix translation of `p` from one side to the other; nullopt
// when no mapping covers it.
std::optional<std::string> translatePath(const std::vector<PathMapping>& maps, const std::string& p,
                                         bool toRemote) {
    std::size_t best = 0;
    std::optional<std::string> out;
    for (const PathMapping& m : maps) {
        const std::string& from = toRemote ? m.local : m.remote;
        const std::string& to = toRemote ? m.remote : m.local;
        std::string rest;
        if (!underRoot(p, from, windowsLike(from), rest)) continue;
        const std::size_t len = rootForm(from).size();
        if (out && len <= best) continue;
        best = len;
        out = joinUnder(to, rest);
    }
    return out;
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// The lines where a list form starts -- `(` or `#(` -- the lines a
// breakpoint can bind to (docs/debugging.md).
std::set<int> formLines(const std::string& source) {
    std::set<int> lines;
    Lexer lexer(source);
    for (;;) {
        const Token t = lexer.next();
        if (t.kind == TokenKind::EndOfFile || t.kind == TokenKind::Error) break;
        if (t.kind == TokenKind::LParen || t.kind == TokenKind::HashLParen) lines.insert(t.line);
    }
    return lines;
}

// ---------------------------------------------------------------------------
// Values, as the Variables view and the Debug Console show them
// ---------------------------------------------------------------------------


constexpr std::size_t kMaxItems  = 100;   // elements printed per collection
constexpr int         kMaxDepth  = 8;     // nesting printed
constexpr std::size_t kMaxLength = 8000;  // characters per value

// Prints `v` readably, as `pr-str` would, but bounded: long or deep
// collections are cut with `...`. Unless `realize` is set, no code of the
// program runs: a lazy seq is shown as far as it is realised (`...` for the
// rest). `realize` (an evaluate's result) realises at most the elements
// shown. `v` must be rooted (a local of a stopped frame, or a slot of the
// caller).
void formatInto(proto::ProtoContext* ctx, std::string& out, const proto::ProtoObject* v, int depth,
                bool realize) {
    if (out.size() > kMaxLength) { out += "..."; return; }
    if (depth > kMaxDepth && (isListValue(ctx, v) || isVector(v) || isMap(v) || isSet(ctx, v) ||
                              isSeqObject(ctx, v))) {
        out += "...";
        return;
    }
    if (isListValue(ctx, v)) {
        out += '(';
        std::size_t i = 0;
        for (const proto::ProtoObject* p = v; p != emptyListValue(); p = listRest(ctx, p), ++i) {
            if (i) out += ' ';
            if (i >= kMaxItems) { out += "..."; break; }
            formatInto(ctx, out, listFirst(ctx, p), depth + 1, realize);
        }
        out += ')';
        return;
    }
    if (isVector(v)) {
        const proto::ProtoList* l = vectorItems(ctx, v);
        const proto::proto_ulong n = l->getSize(ctx);
        out += '[';
        for (proto::proto_ulong i = 0; i < n; ++i) {
            if (i) out += ' ';
            if (i >= kMaxItems) { out += "..."; break; }
            formatInto(ctx, out, l->getAt(ctx, static_cast<int>(i)), depth + 1, realize);
        }
        out += ']';
        return;
    }
    if (isMap(v)) {
        struct Acc { std::string* out; int depth; std::size_t n; bool realize; } acc{&out, depth, 0, realize};
        out += '{';
        mapForEach(ctx, v, &acc, [](proto::ProtoContext* c, void* self,
                                    const proto::ProtoObject* k, const proto::ProtoObject* val) {
            auto* a = static_cast<Acc*>(self);
            if (a->n > kMaxItems) return;
            if (a->n++) *a->out += ", ";
            if (a->n > kMaxItems) { *a->out += "..."; return; }
            formatInto(c, *a->out, k, a->depth + 1, a->realize);
            *a->out += ' ';
            formatInto(c, *a->out, val, a->depth + 1, a->realize);
        });
        out += '}';
        return;
    }
    if (isSet(ctx, v)) {
        struct Acc { std::string* out; int depth; std::size_t n; bool realize; } acc{&out, depth, 0, realize};
        out += "#{";
        setForEach(ctx, v, &acc, [](proto::ProtoContext* c, void* self, const proto::ProtoObject* e) {
            auto* a = static_cast<Acc*>(self);
            if (a->n > kMaxItems) return;
            if (a->n++) *a->out += ' ';
            if (a->n > kMaxItems) { *a->out += "..."; return; }
            formatInto(c, *a->out, e, a->depth + 1, a->realize);
        });
        out += '}';
        return;
    }
    if (isSeqObject(ctx, v)) {
        // Walked without realising anything: the cells are reachable from
        // `v`; the chunks seqMore builds are rooted in this scope's slot.
        proto::ProtoContext scope(ctx->space, ctx);
        scope.resizeAutomaticLocals(1);
        const proto::ProtoObject* node = v;
        std::size_t n = 0;
        out += '(';
        for (;;) {
            if (!node || node == PROTO_NONE) break;
            if (seqObjectKind(&scope, node) == SeqObject::Lazy) {
                if (!realize && !lazyIsRealized(&scope, node)) {
                    if (n) out += ' ';
                    out += "...";
                    break;
                }
                node = realizeLazy(&scope, node);   // unless `realize`, already realised
                scope.setAutomaticLocal(0, node);
                continue;
            }
            if (node == emptyListValue() || !isSeqObject(&scope, node)) break;
            if (n) out += ' ';
            if (n++ >= kMaxItems || out.size() > kMaxLength) { out += "..."; break; }
            formatInto(&scope, out, seqFirst(&scope, node), depth + 1, realize);
            node = seqMore(&scope, node);
            scope.setAutomaticLocal(0, node);
        }
        out += ')';
        return;
    }
    printValueTo(ctx, out, v, /*readable=*/true);
}

std::string formatValue(proto::ProtoContext* ctx, const proto::ProtoObject* v, bool realize = false) {
    std::string out;
    try {
        formatInto(ctx, out, v, 0, realize);
    } catch (const proto::ProtoException&) {
        out += "#<unprintable>";
    } catch (const std::exception&) {
        out += "#<unprintable>";
    }
    return out;
}

// The children the Variables view shows when a value is expanded; empty when
// it has none to show (an atom, a number, an unrealised lazy seq...).
using Children = std::vector<std::pair<std::string, const proto::ProtoObject*>>;

Children childrenOf(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
    Children kids;
    constexpr std::size_t kMaxChildren = 1000;
    if (isListValue(ctx, v)) {
        std::size_t i = 0;
        for (const proto::ProtoObject* p = v; p != emptyListValue() && i < kMaxChildren;
             p = listRest(ctx, p), ++i)
            kids.emplace_back("[" + std::to_string(i) + "]", listFirst(ctx, p));
    } else if (isVector(v)) {
        const proto::ProtoList* l = vectorItems(ctx, v);
        const proto::proto_ulong n = std::min<proto::proto_ulong>(l->getSize(ctx), kMaxChildren);
        for (proto::proto_ulong i = 0; i < n; ++i)
            kids.emplace_back("[" + std::to_string(i) + "]", l->getAt(ctx, static_cast<int>(i)));
    } else if (isMap(v)) {
        struct Acc { Children* kids; } acc{&kids};
        mapForEach(ctx, v, &acc, [](proto::ProtoContext* c, void* self,
                                    const proto::ProtoObject* k, const proto::ProtoObject* val) {
            auto* a = static_cast<Acc*>(self);
            if (a->kids->size() < kMaxChildren) a->kids->emplace_back(formatValue(c, k), val);
        });
    } else if (isSet(ctx, v)) {
        struct Acc { Children* kids; } acc{&kids};
        setForEach(ctx, v, &acc, [](proto::ProtoContext*, void* self, const proto::ProtoObject* e) {
            auto* a = static_cast<Acc*>(self);
            if (a->kids->size() < kMaxChildren)
                a->kids->emplace_back("[" + std::to_string(a->kids->size()) + "]", e);
        });
    }
    return kids;
}

bool hasChildren(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
    if (isListValue(ctx, v)) return v != emptyListValue();
    if (isVector(v)) return vectorItems(ctx, v)->getSize(ctx) > 0;
    if (isMap(v)) return mapCount(ctx, v) > 0;
    if (isSet(ctx, v)) return !setIsEmpty(ctx, v);
    return false;
}

// ---------------------------------------------------------------------------
// Conditions, hit conditions, log messages
// ---------------------------------------------------------------------------

std::string trimmed(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

// A syntax check of an expression the client set, made without running
// anything: the lexer's errors and unbalanced delimiters. Answers "" when
// it reads, else what is wrong.
std::string syntaxProblem(const std::string& expr) {
    if (trimmed(expr).empty()) return "empty expression";
    Lexer lexer(expr);
    std::vector<char> open;
    for (;;) {
        const Token t = lexer.next();
        if (t.kind == TokenKind::EndOfFile) break;
        if (t.kind == TokenKind::Error) return "read error: " + t.text;
        switch (t.kind) {
            case TokenKind::LParen: case TokenKind::HashLParen: open.push_back(')'); break;
            case TokenKind::LBracket: open.push_back(']'); break;
            case TokenKind::LBrace: case TokenKind::HashLBrace: open.push_back('}'); break;
            case TokenKind::RParen: case TokenKind::RBracket: case TokenKind::RBrace: {
                const char c = t.kind == TokenKind::RParen ? ')' : t.kind == TokenKind::RBracket ? ']' : '}';
                if (open.empty() || open.back() != c) return std::string("read error: unexpected ") + c;
                open.pop_back();
                break;
            }
            default: break;
        }
    }
    if (!open.empty()) return std::string("read error: missing ") + open.back();
    return "";
}

// A breakpoint's hitCondition, as VS Code sends it: `N` or `== N` (the Nth
// hit), `>= N`, `> N`, `<= N`, `< N`, `% N` (every Nth hit).
struct HitCondition {
    enum class Op { None, Eq, Ge, Gt, Le, Lt, Mod } op = Op::None;
    long long n = 0;

    // Parses `text`; answers "" or what is wrong with it.
    std::string parse(const std::string& text) {
        std::string t = trimmed(text);
        op = Op::None;
        if (t.empty()) return "";
        Op o = Op::Eq;
        auto starts = [&](const char* p) { return t.rfind(p, 0) == 0; };
        if (starts("==")) { o = Op::Eq; t = t.substr(2); }
        else if (starts(">=")) { o = Op::Ge; t = t.substr(2); }
        else if (starts("<=")) { o = Op::Le; t = t.substr(2); }
        else if (starts(">")) { o = Op::Gt; t = t.substr(1); }
        else if (starts("<")) { o = Op::Lt; t = t.substr(1); }
        else if (starts("%")) { o = Op::Mod; t = t.substr(1); }
        t = trimmed(t);
        if (t.empty() || t.size() > 15 ||
            !std::all_of(t.begin(), t.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return "hit condition '" + text + "': expected N, == N, >= N, > N, <= N, < N or % N";
        n = std::stoll(t);
        if (o == Op::Mod && n == 0) return "hit condition '" + text + "': % 0";
        op = o;
        return "";
    }

    bool passes(long long hits) const {
        switch (op) {
            case Op::None: return true;
            case Op::Eq:   return hits == n;
            case Op::Ge:   return hits >= n;
            case Op::Gt:   return hits > n;
            case Op::Le:   return hits <= n;
            case Op::Lt:   return hits < n;
            case Op::Mod:  return hits % n == 0;
        }
        return true;
    }
};

// A line breakpoint as the client set it.
struct LineBreakpoint {
    std::string  condition;    // "" = none
    HitCondition hit;
    std::string  logMessage;   // "" = a stopping breakpoint
    bool         isLog = false;
    long long    hits = 0;     // hits whose condition passed (guarded by the server's mu_)
};

// ---------------------------------------------------------------------------
// The server
// ---------------------------------------------------------------------------

// Set while a stopped thread evaluates an expression for the client: the
// code it runs never stops.
thread_local bool tEvaluating = false;

// The description a stop gets when a breakpoint condition failed (set and
// taken on the stopping thread).
thread_local std::string tPendingDescription;

// Per thread: the breakpoint lines of each module's source, as of the
// breakpoint generation it was read at, so that a line entered checks a
// small vector instead of taking the server's lock.
struct BreakpointLines {
    std::uint64_t    gen = 0;
    std::vector<int> lines;
};
thread_local std::unordered_map<const void*, BreakpointLines> tBreakpointLines;
thread_local std::uint64_t tBreakpointLinesGen = 0;

bool isLibrary(const DebugFrame* f) {
    const BytecodeModule::DebugInfo* di = f->mod->debugInfo();
    return di && di->library;
}

// A frame id or a variablesReference carries the DAP id of the thread that
// owns it: `threadId * kIdStride + n`. A request about a frame or a value
// reaches the stopped thread whose stack holds it.
constexpr long long kIdStride = 1LL << 24;

enum class StepKind { None, In, Over, Out };
enum class Resume { Continue, StepIn, StepOver, StepOut };

// Why a thread stopped on an exception (exceptionInfo).
struct ExceptionStop {
    std::string typeName;      // the class: ExceptionInfo, ArithmeticException...
    std::string message;       // ex-message, or "" when it has none
    std::string description;   // the exception as the top level reports it
    std::string breakMode;     // "always" (raised) or "userUnhandled"
    std::string stackTrace;    // "  at f (file.clj:12)" lines
};

// What the debugger knows about one thread that runs the program's code.
struct ThreadState {
    int         id = 0;
    std::string name;
    bool        isMain = false;
    std::atomic<bool> pauseRequested{false};

    // The thread's own: read and written only by the thread itself.
    StepKind stepKind = StepKind::None;
    int      stepDepth = 0;
    bool     stepFromLibrary = false;   // the step began in clojure.core

    // Guarded by the server's mu_.
    bool             stopped = false, resumeReady = false;
    Resume           resume = Resume::Continue;
    std::deque<json> requests;           // inspection requests for the thread
    bool             hasException = false;
    ExceptionStop    exception;

    // The thread's own while it is stopped: frame ids and variable
    // references index these.
    struct Ref {
        DebugFrame* frame;                   // the Locals of this frame...
        const proto::ProtoObject* value;     // ...or this value's children
    };
    DebugFrame*              stopFrame = nullptr;
    std::vector<DebugFrame*> frames;     // id % kIdStride - 1 -> frame
    std::vector<Ref>         refs;       // reference % kIdStride - 1 -> what
};

class DapServer;

// The server threads report their end to, guarded by serverMu() (never
// destroyed: a thread may end after the server).
std::mutex& serverMu() {
    static std::mutex* mu = new std::mutex;
    return *mu;
}
DapServer* gServer = nullptr;

// The calling thread's registration with the server, and the report of its
// end: the destructor of a thread_local runs when the thread ends.
thread_local ThreadState* tState = nullptr;
struct ThreadExit {
    int id = 0;
    ~ThreadExit();
};
thread_local ThreadExit tExit;

class DapServer final : public DebugHook {
public:
    DapServer() : transport_(stdio_.protocolIn(), stdio_.protocolOut()) {
        std::lock_guard<std::mutex> lk(serverMu());
        gServer = this;
    }
    ~DapServer() override {
        std::lock_guard<std::mutex> lk(serverMu());
        gServer = nullptr;
    }

    int run(int (*runProgram)(const DapLaunch&, void*), void* arg) {
        // What the program prints reaches the client as `output` events
        // instead of corrupting the protocol stream (DapStdio.h).
        stdio_.startForwarding([this](const char* category, const std::string& text) {
            sendOutput(category, text);
        });
        reader_ = std::thread(&DapServer::readLoop, this);

        bool start = false;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return (launched_ && configDone_) || inputClosed_ || disconnected_; });
            start = launched_ && configDone_ && !disconnected_;
            if (start) running_ = true;
        }
        int rc = 0;
        if (start) {
            programKey_ = pathKey(launch_.program);
            entryPending_.store(launch_.stopOnEntry);
            if (!cwd_.empty()) {
                std::error_code ec;
                std::filesystem::current_path(cwd_, ec);
                if (ec) sendOutput("stderr", "protoclj --dap: cannot change to cwd " + cwd_ + ": " + ec.message() + "\n");
            }
            setDebugHook(this);
            rc = runProgram(launch_, arg);
            setDebugHook(nullptr);
            {
                std::lock_guard<std::mutex> lk(mu_);
                running_ = false;
            }
            stdio_.finish(nullptr);          // the program's output goes first
            sendEvent("terminated", json::object());
            sendEvent("exited", {{"exitCode", rc}});
        }

        // The client answers `terminated` with `disconnect`.
        bool readerDone = false;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait_for(lk, std::chrono::seconds(10),
                         [&] { return readerDone_; });
            readerDone = readerDone_;
        }
        if (readerDone) {
            // The program's space is gone by now; the bracket (on no
            // context) keeps the rule that every join is one.
            proto::ProtoContext::UnmanagedScope unmanaged(nullptr);
            reader_.join();
        } else {
            // Still blocked reading a client that does not hang up.
            std::fflush(nullptr);
            std::_Exit(rc);
        }
        return rc;
    }

    // --- DebugHook -----------------------------------------------------------
    void onLine(proto::ProtoContext* ctx, DebugFrame* frame) override {
        if (tEvaluating) return;
        ThreadState* ts = tState;
        if (!ts) [[unlikely]] ts = registerThread(frame);
        const bool entry = ts->isMain && entryPending_.load(std::memory_order_relaxed);
        if (!anyBreakpoints_.load(std::memory_order_relaxed) && ts->stepKind == StepKind::None &&
            !ts->pauseRequested.load(std::memory_order_relaxed) && !entry)
            return;

        // clojure.core's lines: never the entry stop (core loads first); with
        // justMyCode no pause or step stops there either, unless the step
        // began in core. A breakpoint set in core always stops.
        const bool library = isLibrary(frame);
        const bool skipLibrary = library && justMyCode_;
        const char* reason = nullptr;
        if (entry && !library && entryPending_.exchange(false)) {
            reason = "entry";
        } else if (!skipLibrary && ts->pauseRequested.exchange(false)) {
            reason = "pause";
        } else {
            if (ts->stepKind != StepKind::None && !(skipLibrary && !ts->stepFromLibrary)) {
                const int depth = depthOf(frame);
                switch (ts->stepKind) {
                    case StepKind::None: break;
                    case StepKind::In:   reason = "step"; break;
                    case StepKind::Over: if (depth <= ts->stepDepth) reason = "step"; break;
                    case StepKind::Out:  if (depth <  ts->stepDepth) reason = "step"; break;
                }
            }
            if (!reason && anyBreakpoints_.load() && breakpointLineSet(frame)) {
                LineBreakpoint bp;
                if (breakpointAt(frame, bp)) {
                    if (breakpointHit(ctx, frame, bp)) return;   // a log point, or it does not stop
                    if (!tPendingDescription.empty()) {
                        stop(ctx, ts, frame, "breakpoint", nullptr, takeDescription());
                        return;
                    }
                    reason = "breakpoint";
                }
            }
        }
        if (reason) stop(ctx, ts, frame, reason, nullptr);
    }

    void onThrow(proto::ProtoContext* ctx, const proto::ProtoObject* value, ThrowSite site) override {
        if (tEvaluating) return;
        const bool raised = exRaised_.load(std::memory_order_relaxed);
        const bool uncaught = exUncaught_.load(std::memory_order_relaxed);
        if (!raised && !uncaught) return;
        DebugFrame* top = debugTop();
        if (!top) return;
        // With justMyCode the stop shows the program's innermost frame; a
        // throw with none (clojure.core loading) does not stop.
        DebugFrame* shown = top;
        if (justMyCode_)
            while (shown && isLibrary(shown)) shown = shown->parent;
        if (!shown) return;
        ThreadState* ts = tState;
        if (!ts) ts = registerThread(shown);

        // The value stays rooted here while the thread is stopped.
        proto::ProtoContext scope(ctx->space, ctx);
        scope.resizeAutomaticLocals(1);
        scope.setAutomaticLocal(0, value);
        std::string raisedCond, uncaughtCond;
        {
            std::lock_guard<std::mutex> lk(mu_);
            raisedCond = exRaisedCondition_;
            uncaughtCond = exUncaughtCondition_;
        }
        ExceptionStop ex;
        std::string conditionError;
        // A filter's condition, evaluated in the throwing frame with the
        // exception bound to `ex`: true when the filter stops.
        auto conditionPasses = [&](const std::string& cond) {
            if (cond.empty()) return true;
            std::string error;
            bool truthy = false;
            if (!evalInFrame(&scope, shown, cond, scope.getAutomaticLocal(0), error,
                             [&](proto::ProtoContext*, const proto::ProtoObject* v) {
                                 truthy = v != PROTO_NONE && v != PROTO_FALSE && v != nullptr;
                             })) {
                conditionError = "exception breakpoint condition `" + cond + "` failed: " + error;
                return true;   // never skipped silently: the stop shows the error
            }
            return truthy;
        };
        if (raised && conditionPasses(raisedCond)) {
            ex.breakMode = "always";
        } else if (uncaught && !caughtByProgram(&scope, top, value) && conditionPasses(uncaughtCond)) {
            ex.breakMode = "userUnhandled";
        } else {
            return;
        }
        describeException(&scope, value, site, top, ex);
        if (shown != top && site == ThrowSite::Throw)
            ex.description += " (thrown in clojure.core, called on this line)";
        if (!conditionError.empty()) sendOutput("console", conditionError + "\n");
        stop(&scope, ts, shown, "exception", &ex, conditionError);
    }

    // A thread that ran the program's code ended (ThreadExit).
    void threadExited(int id) {
        bool known = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            known = threads_.erase(id) > 0;
        }
        if (known) sendEvent("thread", {{"reason", "exited"}, {"threadId", id}});
    }

private:
    static int depthOf(const DebugFrame* f) {
        int d = 0;
        for (; f; f = f->parent) ++d;
        return d;
    }

    // Whether some breakpoint is on the line `frame` entered, from the
    // thread's cache of its source's breakpoint lines.
    bool breakpointLineSet(const DebugFrame* frame) {
        const BytecodeModule::DebugInfo* di = frame->mod->debugInfo();
        if (!di) return false;
        const std::uint64_t gen = bpGen_.load(std::memory_order_acquire);
        if (tBreakpointLinesGen != gen) {
            tBreakpointLines.clear();
            tBreakpointLinesGen = gen;
        }
        BreakpointLines& e = tBreakpointLines[di];
        if (e.gen != gen) {
            e.lines.clear();
            std::lock_guard<std::mutex> lk(mu_);
            auto it = breakpoints_.find(di->source == launch_.program ? programKey_ : pathKey(di->source));
            if (it != breakpoints_.end())
                for (const auto& [line, bp] : it->second) e.lines.push_back(line);
            e.gen = gen;
        }
        return std::find(e.lines.begin(), e.lines.end(), frame->line) != e.lines.end();
    }

    // Whether a breakpoint is set on the line `frame` entered; a copy of it.
    bool breakpointAt(const DebugFrame* frame, LineBreakpoint& out) {
        const BytecodeModule::DebugInfo* di = frame->mod->debugInfo();
        if (!di) return false;
        std::lock_guard<std::mutex> lk(mu_);
        auto it = breakpoints_.find(di->source == launch_.program ? programKey_ : pathKey(di->source));
        if (it == breakpoints_.end()) return false;
        auto at = it->second.find(frame->line);
        if (at == it->second.end()) return false;
        out = at->second;
        return true;
    }

    // Counts a hit of `bp` (the breakpoint on frame's line) whose condition
    // passed; answers its new count.
    long long countHit(const DebugFrame* frame) {
        const BytecodeModule::DebugInfo* di = frame->mod->debugInfo();
        std::lock_guard<std::mutex> lk(mu_);
        auto it = breakpoints_.find(di->source == launch_.program ? programKey_ : pathKey(di->source));
        if (it == breakpoints_.end()) return 1;
        auto at = it->second.find(frame->line);
        return at == it->second.end() ? 1 : ++at->second.hits;
    }

    // The thread is on a line with breakpoint `bp`: its condition, hit
    // condition and log message, evaluated on this thread in `frame`.
    // Answers true when the thread does not stop. A condition that fails
    // stops, with the failure as the stop's description (tPendingDescription).
    bool breakpointHit(proto::ProtoContext* ctx, DebugFrame* frame, const LineBreakpoint& bp) {
        tPendingDescription.clear();
        if (!bp.condition.empty()) {
            std::string error;
            bool truthy = false;
            if (!evalInFrame(ctx, frame, bp.condition, nullptr, error,
                             [&](proto::ProtoContext*, const proto::ProtoObject* v) {
                                 truthy = v != PROTO_NONE && v != PROTO_FALSE && v != nullptr;
                             })) {
                tPendingDescription = "breakpoint condition `" + bp.condition + "` failed: " + error;
                sendOutput("console", tPendingDescription + "\n");
                return false;
            }
            if (!truthy) return true;
        }
        const long long hits = countHit(frame);
        if (!bp.hit.passes(hits)) return true;
        if (bp.isLog) {
            sendOutput("console", interpolate(ctx, frame, bp.logMessage) + "\n");
            return true;
        }
        return false;
    }

    // The log message with each `{expr}` replaced by its value in `frame`;
    // `{{` and `}}` are literal braces, an expression that fails reads
    // `<error: message>`.
    std::string interpolate(proto::ProtoContext* ctx, DebugFrame* frame, const std::string& msg) {
        std::string out;
        for (std::size_t i = 0; i < msg.size();) {
            const char c = msg[i];
            if (c == '{' && i + 1 < msg.size() && msg[i + 1] == '{') { out += '{'; i += 2; continue; }
            if (c == '}' && i + 1 < msg.size() && msg[i + 1] == '}') { out += '}'; i += 2; continue; }
            if (c != '{') { out += c; ++i; continue; }
            // The expression runs to the matching close brace (a map literal
            // inside it nests).
            std::size_t j = i + 1;
            int depth = 1;
            bool inString = false;
            for (; j < msg.size(); ++j) {
                const char d = msg[j];
                if (inString) {
                    if (d == '\\' && j + 1 < msg.size()) ++j;
                    else if (d == '"') inString = false;
                    continue;
                }
                if (d == '"') inString = true;
                else if (d == '{') ++depth;
                else if (d == '}' && --depth == 0) break;
            }
            if (j >= msg.size()) { out += msg.substr(i); break; }   // no close: literal text
            const std::string expr = msg.substr(i + 1, j - i - 1);
            std::string error;
            if (!evalInFrame(ctx, frame, expr, nullptr, error,
                             [&](proto::ProtoContext* c, const proto::ProtoObject* v) {
                                 if (v && v != PROTO_NONE && proto::ProtoObject::isStringTagFast(v)) printValueTo(c, out, v, /*readable=*/false);
                                 else out += formatValue(c, v, /*realize=*/true);
                             }))
                out += "<error: " + error + ">";
            i = j + 1;
        }
        return out;
    }

    // The description of a stop a failing condition caused, taken once.
    std::string takeDescription() {
        std::string d;
        d.swap(tPendingDescription);
        return d;
    }

    // --- threads ---------------------------------------------------------------

    // The calling thread's first line (or throw): it gets an id and a name,
    // and the client a `thread` event. The evaluator thread is "main", id 1.
    ThreadState* registerThread(const DebugFrame* frame) {
        auto ts = std::make_unique<ThreadState>();
        const char* kind = debugThreadKind();
        ThreadState* raw = ts.get();
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!kind && !mainSeen_) {
                mainSeen_ = true;
                ts->id = 1;
                ts->isMain = true;
                ts->name = "main";
            } else {
                ts->id = nextThreadId_++;
                if (!kind) {
                    ts->name = "thread-" + std::to_string(ts->id);
                } else if (std::string(kind) == "actor") {
                    // A worker runs the turns of every actor in turn.
                    ts->name = "actor worker " + std::to_string(++actorWorkers_);
                } else {
                    const BytecodeModule::DebugInfo* di = frame->mod->debugInfo();
                    const int line = frame->line ? frame->line : (di ? di->line : 0);
                    ts->name = std::string(kind) + "@" + (di ? baseName(di->source) : std::string("?")) +
                               ":" + std::to_string(line);
                }
            }
            threads_[ts->id] = std::move(ts);
        }
        tState = raw;
        tExit.id = raw->id;
        sendEvent("thread", {{"reason", "started"}, {"threadId", raw->id}});
        return raw;
    }

    // Under mu_.
    ThreadState* threadById(long long id) {
        auto it = threads_.find(static_cast<int>(id));
        return it == threads_.end() ? nullptr : it->second.get();
    }

    // --- exceptions --------------------------------------------------------------

    // Whether a try of the program's frames, from the innermost out, catches
    // `value`. The walk ends at a native site that swallows what it calls
    // throws (DebugCatchBarrier): the program never sees the exception.
    static bool caughtByProgram(proto::ProtoContext* ctx, const DebugFrame* top,
                                const proto::ProtoObject* value) {
        const DebugFrame* barrierTop = nullptr;
        const bool barrier = debugCatchBarrier(barrierTop);
        for (const DebugFrame* f = top; f; f = f->parent) {
            if (barrier && f == barrierTop) return false;
            if (debugFrameCatches(ctx, f, value)) return true;
        }
        return false;
    }

    static void describeException(proto::ProtoContext* ctx, const proto::ProtoObject* v,
                                  ThrowSite site, const DebugFrame* top, ExceptionStop& ex) {
        try {
            if (isException(ctx, v)) ex.typeName = exceptionClassName(exceptionClassOf(ctx, v));
            else ex.typeName = valueTypeName(ctx, v);
            const proto::ProtoObject* m = thrownMessage(ctx, v);
            if (m && m != PROTO_NONE) printValueTo(ctx, ex.message, m, /*readable=*/false);
            ex.description = describeThrown(ctx, v);
        } catch (...) {
            if (ex.typeName.empty()) ex.typeName = "Throwable";
            if (ex.description.empty()) ex.description = ex.typeName;
        }
        if (site != ThrowSite::Throw)
            ex.description += site == ThrowSite::Kernel
                ? " (raised by protoCore in a call on this line)"
                : " (raised by the runtime in a call on this line)";
        for (const DebugFrame* f = top; f; f = f->parent) {
            const BytecodeModule::DebugInfo* di = f->mod->debugInfo();
            ex.stackTrace += "  at " + di->name + " (" + baseName(di->source) + ":" +
                             std::to_string(f->shownLine) + ")\n";
        }
    }

    // --- stopping ------------------------------------------------------------------

    // The thread stops: tell the client, then answer its inspection requests
    // until the client resumes this thread. The other threads run on. Runs on
    // the stopped thread.
    void stop(proto::ProtoContext* ctx, ThreadState* ts, DebugFrame* frame, const char* reason,
              const ExceptionStop* ex, const std::string& description = std::string()) {
        // A line printed without a newline is still in stdio's buffer.
        std::fflush(stdout);
        std::fflush(stderr);
        std::cout.flush();
        {
            // What the program printed before the stop reaches the client
            // before the `stopped` event. Unmanaged: a collection never
            // waits for this thread.
            proto::ProtoContext::UnmanagedScope unmanaged(ctx);
            stdio_.waitForwarded();
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (disconnected_) return;
            ts->stopped = true;
            ts->resumeReady = false;
            ts->stepKind = StepKind::None;
            ts->stopFrame = frame;
            ts->frames.clear();
            ts->refs.clear();
            ts->hasException = ex != nullptr;
            if (ex) ts->exception = *ex;
            lastStopped_ = ts->id;
        }
        json body = {{"reason", reason}, {"threadId", ts->id}, {"allThreadsStopped", false}};
        if (ex) {
            body["description"] = ex->description;
            body["text"] = ex->typeName;
        }
        if (!description.empty()) {
            body["description"] = description;
            if (!ex) body["text"] = description;
        }
        sendEvent("stopped", body);

        for (;;) {
            json req;
            bool resume = false;
            {
                proto::ProtoContext::UnmanagedScope unmanaged(ctx);
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [&] { return ts->resumeReady || !ts->requests.empty() || disconnected_; });
                if (!ts->requests.empty()) {
                    req = std::move(ts->requests.front());
                    ts->requests.pop_front();
                } else {
                    resume = true;
                }
            }
            if (resume) break;
            answerStopped(ctx, ts, req);
        }

        std::deque<json> late;
        {
            std::lock_guard<std::mutex> lk(mu_);
            ts->stopped = false;
            ts->hasException = false;
            late.swap(ts->requests);
            ts->frames.clear();
            ts->refs.clear();
            ts->stopFrame = nullptr;
            const Resume r = disconnected_ ? Resume::Continue : ts->resume;
            switch (r) {
                case Resume::Continue: ts->stepKind = StepKind::None; break;
                case Resume::StepIn:   ts->stepKind = StepKind::In;   break;
                case Resume::StepOver: ts->stepKind = StepKind::Over; break;
                case Resume::StepOut:  ts->stepKind = StepKind::Out;  break;
            }
            ts->stepDepth = depthOf(frame);
            ts->stepFromLibrary = isLibrary(frame);
        }
        for (const json& r : late) sendErrorResponse(r, "the thread is running");
    }

    // --- inspection, on the stopped thread -----------------------------------

    // The locals of `f` in scope where it stopped: the innermost binding of
    // each name, in slot order.
    static std::vector<const BytecodeModule::LocalVar*> visibleLocals(const DebugFrame* f) {
        std::vector<const BytecodeModule::LocalVar*> out;
        const BytecodeModule::DebugInfo* di = f->mod->debugInfo();
        if (!di) return out;
        std::map<std::string, const BytecodeModule::LocalVar*> byName;
        for (const auto& lv : di->locals) {
            if (lv.startPc > f->pc || f->pc >= lv.endPc) continue;
            auto [it, fresh] = byName.emplace(lv.name, &lv);
            if (!fresh && lv.startPc >= it->second->startPc) it->second = &lv;
        }
        for (const auto& [name, lv] : byName) out.push_back(lv);
        std::sort(out.begin(), out.end(), [](auto* a, auto* b) { return a->slot < b->slot; });
        return out;
    }

    void answerStopped(proto::ProtoContext* ctx, ThreadState* ts, const json& req) {
        const std::string command = req.value("command", "");
        const json args = req.value("arguments", json::object());
        try {
            if (command == "stackTrace")     stackTrace(ts, req, args);
            else if (command == "scopes")    scopes(ts, req, args);
            else if (command == "variables") variables(ctx, ts, req, args);
            else if (command == "evaluate")  evaluate(ctx, ts, req, args);
            else sendResponse(req, json::object());
        } catch (const std::exception& e) {
            sendErrorResponse(req, e.what());
        }
    }

    void stackTrace(ThreadState* ts, const json& req, const json& args) {
        const int start = args.value("startFrame", 0);
        const int levels = args.value("levels", 0);
        std::vector<DebugFrame*> stack;
        for (DebugFrame* f = ts->stopFrame; f; f = f->parent) {
            // A top-level let, loop or try runs as a fn of its own; the
            // form's module that calls it is not shown again.
            if (f->parent && f->mod->debugInfo()->topLevelThunk) {
                stack.push_back(f);
                f = f->parent;
                continue;
            }
            stack.push_back(f);
        }
        json frames = json::array();
        const int total = static_cast<int>(stack.size());
        for (int i = start; i < total; ++i) {
            if (levels > 0 && static_cast<int>(frames.size()) >= levels) break;
            DebugFrame* f = stack[static_cast<std::size_t>(i)];
            ts->frames.push_back(f);
            const long long id = ts->id * kIdStride + static_cast<long long>(ts->frames.size());
            const BytecodeModule::DebugInfo* di = f->mod->debugInfo();
            frames.push_back({{"id", id},
                              {"name", di->name},
                              {"line", f->shownLine},
                              {"column", 1},
                              {"source", sourceFor(di->source)}});
            if (di->library) frames.back()["presentationHint"] = "subtle";
        }
        sendResponse(req, {{"stackFrames", frames}, {"totalFrames", total}});
    }

    static DebugFrame* frameById(ThreadState* ts, long long id) {
        const long long n = id % kIdStride;
        if (id / kIdStride == ts->id && n >= 1 && static_cast<std::size_t>(n) <= ts->frames.size())
            return ts->frames[static_cast<std::size_t>(n) - 1];
        return ts->stopFrame;
    }

    static long long newRef(ThreadState* ts, ThreadState::Ref r) {
        ts->refs.push_back(r);
        return ts->id * kIdStride + static_cast<long long>(ts->refs.size());
    }

    void scopes(ThreadState* ts, const json& req, const json& args) {
        DebugFrame* f = frameById(ts, args.value("frameId", 0LL));
        json list = json::array();
        if (f) {
            list.push_back({{"name", "Locals"},
                            {"presentationHint", "locals"},
                            {"variablesReference", newRef(ts, {f, nullptr})},
                            {"expensive", false}});
        }
        sendResponse(req, {{"scopes", list}});
    }

    json variable(proto::ProtoContext* ctx, ThreadState* ts, const std::string& name,
                  const proto::ProtoObject* v) {
        // A value reachable from a stopped frame's local stays alive while
        // the thread is stopped, so its children can be listed later.
        const long long ref = hasChildren(ctx, v) ? newRef(ts, {nullptr, v}) : 0;
        return {{"name", name}, {"value", formatValue(ctx, v)}, {"variablesReference", ref}};
    }

    void variables(proto::ProtoContext* ctx, ThreadState* ts, const json& req, const json& args) {
        const long long ref = args.value("variablesReference", 0LL);
        const long long n = ref % kIdStride;
        json list = json::array();
        if (ref / kIdStride == ts->id && n >= 1 && static_cast<std::size_t>(n) <= ts->refs.size()) {
            const ThreadState::Ref r = ts->refs[static_cast<std::size_t>(n) - 1];
            if (r.frame) {
                for (const auto* lv : visibleLocals(r.frame))
                    list.push_back(variable(ctx, ts, lv->name,
                                            r.frame->ctx->getAutomaticLocal(static_cast<unsigned>(lv->slot))));
            } else if (r.value) {
                for (const auto& [name, v] : childrenOf(ctx, r.value)) list.push_back(variable(ctx, ts, name, v));
            }
        }
        sendResponse(req, {{"variables", list}});
    }

    // Whether `name` occurs in `expr` as a whole symbol.
    static bool mentions(const std::string& expr, const std::string& name) {
        auto delim = [](char c) {
            return std::isspace(static_cast<unsigned char>(c)) || c == '(' || c == ')' || c == '[' ||
                   c == ']' || c == '{' || c == '}' || c == '"' || c == '\'' || c == '`' ||
                   c == '~' || c == '@' || c == '^' || c == ';' || c == ',';
        };
        for (std::size_t at = expr.find(name); at != std::string::npos; at = expr.find(name, at + 1)) {
            const bool before = at == 0 || delim(expr[at - 1]);
            const std::size_t end = at + name.size();
            const bool after = end == expr.size() || delim(expr[end]);
            if (before && after) return true;
        }
        return false;
    }

    // Evaluates `expr` in frame `f` on this thread: the frame's locals the
    // expression names are bound to their values, as the parameters of
    // `(fn [ex? locals...] expr)`, compiled and called here; `exValue`, when
    // given, is bound to `ex` (shadowing a local of that name). Breakpoints,
    // steps and exception stops are off on the thread while it runs
    // (tEvaluating). On success calls use(ctx, value) with the value rooted
    // and answers true; else answers false with `error` set.
    template <class Use>
    bool evalInFrame(proto::ProtoContext* ctx, DebugFrame* f, const std::string& expression,
                     const proto::ProtoObject* exValue, std::string& error, Use&& use) {
        const std::string expr = trimmed(expression);
        if (expr.empty()) { error = "nothing to evaluate"; return false; }
        const ActiveCallContext* cc = activeCallContext();
        if (!cc || !cc->engine) { error = "no evaluator on this thread"; return false; }

        std::vector<const BytecodeModule::LocalVar*> used;
        if (f) {
            for (const auto* lv : visibleLocals(f)) {
                if (exValue && lv->name == "ex") continue;
                if (lv->name.find('#') == std::string::npos && lv->name[0] != '&' && mentions(expr, lv->name))
                    used.push_back(lv);
            }
        }
        const std::size_t extra = exValue ? 1 : 0;
        if (used.size() + extra > 16) { error = "the expression names more than 16 locals"; return false; }

        std::string src = "(fn [";
        if (exValue) src += "ex ";
        for (const auto* lv : used) src += lv->name + " ";
        src += "]\n" + expr + "\n)";

        const bool wasEvaluating = tEvaluating;
        tEvaluating = true;
        struct Reset { bool was; ~Reset() { tEvaluating = was; } } reset{wasEvaluating};
        proto::ProtoContext scope(ctx->space, ctx);
        scope.resizeAutomaticLocals(2);
        try {
            Reader reader(&scope, src, cc->named);
            reader.setSymbolResolver(&qualifySyntaxQuoteSymbol);
            scope.setAutomaticLocal(0, reader.readOne(&scope));
            scope.setAutomaticLocal(1, evalTopLevel(&scope, scope.getAutomaticLocal(0), nullptr));
            const proto::ProtoObject* argv[16];
            if (exValue) argv[0] = exValue;
            for (std::size_t i = 0; i < used.size(); ++i)
                argv[extra + i] = f->ctx->getAutomaticLocal(static_cast<unsigned>(used[i]->slot));
            scope.setAutomaticLocal(0, cc->engine->invoke(&scope, scope.getAutomaticLocal(1), argv,
                                                          static_cast<unsigned>(used.size() + extra)));
            use(&scope, scope.getAutomaticLocal(0));
            return true;
        } catch (const ReaderError& e) {
            error = std::string("read error: ") + e.what();
        } catch (const CompileError& e) {
            error = std::string("compile error: ") + e.what();
        } catch (const proto::ProtoException& e) {
            error = describeThrown(&scope, e.value());
        } catch (const std::exception& e) {
            error = e.what();
        }
        return false;
    }

    // Evaluates `expression` in the selected frame (Watch, Debug Console,
    // hover).
    void evaluate(proto::ProtoContext* ctx, ThreadState* ts, const json& req, const json& args) {
        const std::string expr = trimmed(args.value("expression", ""));
        if (expr.empty()) { sendErrorResponse(req, "nothing to evaluate"); return; }
        DebugFrame* f = frameById(ts, args.value("frameId", 0LL));
        if (f) {
            for (const auto* lv : visibleLocals(f)) {
                if (lv->name == expr) {       // a local by itself: its value
                    sendResponse(req, variableBody(ctx, ts, f->ctx->getAutomaticLocal(static_cast<unsigned>(lv->slot)), true));
                    return;
                }
            }
        }
        std::string error;
        if (evalInFrame(ctx, f, expr, nullptr, error, [&](proto::ProtoContext* c, const proto::ProtoObject* v) {
                sendResponse(req, variableBody(c, ts, v, false));
            }))
            return;
        sendErrorResponse(req, error);
    }

    // An evaluate response's body. Only a value that stays alive while the
    // thread is stopped -- a local, or part of one -- can be expanded.
    json variableBody(proto::ProtoContext* ctx, ThreadState* ts, const proto::ProtoObject* v, bool rooted) {
        const long long ref = rooted && hasChildren(ctx, v) ? newRef(ts, {nullptr, v}) : 0;
        return {{"result", formatValue(ctx, v, !rooted)}, {"variablesReference", ref}};
    }

    // --- sources -----------------------------------------------------------------

    // The adapter-side path of a client path: translated by pathMappings when
    // one covers it, else as given.
    std::string toAdapterPath(const std::string& clientPath) const {
        if (auto t = translatePath(mappings_, clientPath, /*toRemote=*/true)) return *t;
        return clientPath;
    }

    // The stable sourceReference of an adapter-side source.
    long long referenceFor(const std::string& adapter) {
        std::lock_guard<std::mutex> lk(srcMu_);
        auto it = sourceRefs_.find(adapter);
        if (it != sourceRefs_.end()) return it->second;
        refSources_.push_back(adapter);
        const long long ref = static_cast<long long>(refSources_.size());
        sourceRefs_[adapter] = ref;
        return ref;
    }

    std::optional<std::string> sourceOfReference(long long ref) {
        std::lock_guard<std::mutex> lk(srcMu_);
        if (ref < 1 || static_cast<std::size_t>(ref) > refSources_.size()) return std::nullopt;
        return refSources_[static_cast<std::size_t>(ref) - 1];
    }

    static bool isEmbedded(const std::string& adapter) { return adapter == embeddedCoreSourceName(); }

    // How the client sees an adapter-side source: a path it can open, or a
    // sourceReference whose text the `source` request serves (clojure.core,
    // embedded in the binary; a file no pathMapping covers).
    json sourceFor(const std::string& adapter) {
        if (isEmbedded(adapter))
            return {{"name", "core.clj"},
                    {"sourceReference", referenceFor(adapter)},
                    {"origin", "clojure.core (embedded in protoclj)"},
                    {"presentationHint", "deemphasize"}};
        const std::string name = baseName(adapter);
        if (mappings_.empty()) return {{"name", name}, {"path", adapter}};
        if (auto local = translatePath(mappings_, adapter, /*toRemote=*/false))
            return {{"name", name}, {"path", *local}};
        return {{"name", name}, {"sourceReference", referenceFor(adapter)}, {"origin", adapter}};
    }

    // The adapter-side source a client `source` object names: by its
    // sourceReference, else by its path.
    std::optional<std::string> adapterSourceOf(const json& source) {
        const long long ref = source.is_object() ? source.value("sourceReference", 0LL) : 0;
        if (ref > 0) return sourceOfReference(ref);
        const std::string path = source.is_object() ? source.value("path", "") : "";
        if (path.empty()) return std::nullopt;
        return toAdapterPath(path);
    }

    static bool sourceText(const std::string& adapter, std::string& out) {
        if (isEmbedded(adapter)) { out = embeddedCoreSource(); return true; }
        return readFile(adapter, out);
    }

    // A source the program loaded: listed by loadedSources, announced once.
    void sourceLoaded(const std::string& adapter) {
        {
            std::lock_guard<std::mutex> lk(srcMu_);
            if (std::find(loaded_.begin(), loaded_.end(), adapter) != loaded_.end()) return;
            loaded_.push_back(adapter);
        }
        sendEvent("loadedSource", {{"reason", "new"}, {"source", sourceFor(adapter)}});
    }

    // --- the protocol thread ------------------------------------------------

    void readLoop() {
        for (;;) {
            std::optional<json> msg = transport_.readMessage();
            if (!msg) break;
            if (msg->value("type", "") != "request") continue;
            if (handle(*msg)) break;
        }
        bool running = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            inputClosed_ = true;
            readerDone_ = true;
            running = running_;
        }
        cv_.notify_all();
        if (running) {
            // The client went away (disconnect, or its end of the pipe
            // closed) while the program runs: end the program with it.
            std::fflush(nullptr);
            std::_Exit(0);
        }
    }

    // Queues `req` for the thread `threadId` if it is stopped (it answers it
    // itself); false if it is not.
    bool queueFor(long long threadId, const json& req) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            ThreadState* ts = threadById(threadId);
            if (!ts || !ts->stopped || ts->resumeReady) return false;
            ts->requests.push_back(req);
        }
        cv_.notify_all();
        return true;
    }

    // Answers true when the session is over.
    bool handle(const json& req) {
        const std::string command = req.value("command", "");
        const json args = req.value("arguments", json::object());

        if (command == "initialize") {
            json filters = json::array();
            const char* condition =
                "A Clojure form; `ex` is the exception, e.g. (= \"bad\" (ex-message ex)) or "
                "(contains? (ex-data ex) :id)";
            filters.push_back({{"filter", "raised"},
                               {"label", "All Exceptions"},
                               {"description", "Stop at every throw, caught or not."},
                               {"default", false},
                               {"supportsCondition", true},
                               {"conditionDescription", condition}});
            filters.push_back({{"filter", "userUnhandled"},
                               {"label", "Uncaught Exceptions"},
                               {"description", "Stop at a throw no try/catch of the program catches."},
                               {"default", false},
                               {"supportsCondition", true},
                               {"conditionDescription", condition}});
            sendResponse(req, {{"supportsConfigurationDoneRequest", true},
                               {"supportsEvaluateForHovers", true},
                               {"supportsTerminateRequest", true},
                               {"supportsExceptionInfoRequest", true},
                               {"supportsExceptionFilterOptions", true},
                               {"supportsConditionalBreakpoints", true},
                               {"supportsHitConditionalBreakpoints", true},
                               {"supportsLogPoints", true},
                               {"supportsLoadedSourcesRequest", true},
                               {"exceptionBreakpointFilters", filters}});
            sendEvent("initialized", json::object());
            return false;
        }
        if (command == "launch") {
            DapLaunch l;
            std::vector<PathMapping> maps;
            if (args.contains("pathMappings")) {
                const json& pm = args["pathMappings"];
                std::string problem;
                if (!pm.is_array()) problem = "launch: pathMappings must be an array of {localRoot, remoteRoot}";
                else
                    for (std::size_t i = 0; i < pm.size() && problem.empty(); ++i) {
                        const json& m = pm[i];
                        const std::string lr = m.is_object() && m.contains("localRoot") && m["localRoot"].is_string()
                                                   ? m["localRoot"].get<std::string>() : "";
                        const std::string rr = m.is_object() && m.contains("remoteRoot") && m["remoteRoot"].is_string()
                                                   ? m["remoteRoot"].get<std::string>() : "";
                        if (lr.empty() || rr.empty())
                            problem = "launch: pathMappings[" + std::to_string(i) +
                                      "] needs both localRoot and remoteRoot (non-empty strings)";
                        else
                            maps.push_back({lr, rr});
                    }
                if (!problem.empty()) {
                    sendErrorResponse(req, problem);
                    sendEvent("terminated", json::object());
                    return false;
                }
            }
            mappings_ = maps;
            justMyCode_ = args.value("justMyCode", true);
            l.program = toAdapterPath(args.value("program", ""));
            l.stopOnEntry = args.value("stopOnEntry", false);
            const std::string cwd = args.value("cwd", "");
            if (!cwd.empty()) {
                cwd_ = toAdapterPath(cwd);
                std::error_code ec;
                if (!std::filesystem::is_directory(cwd_, ec)) {
                    sendErrorResponse(req, "launch: cwd " + cwd_ + " is not a directory");
                    sendEvent("terminated", json::object());
                    return false;
                }
            }
            if (args.contains("args") && args["args"].is_array())
                for (const auto& a : args["args"])
                    if (a.is_string()) l.args.push_back(a.get<std::string>());
            std::string source;
            if (l.program.empty()) {
                sendErrorResponse(req, "launch: no 'program' to debug");
                sendEvent("terminated", json::object());
                return false;
            }
            if (!readFile(l.program, source)) {
                sendErrorResponse(req, "launch: cannot open " + l.program);
                sendEvent("terminated", json::object());
                return false;
            }
            {
                std::lock_guard<std::mutex> lk(mu_);
                launch_ = l;
                launched_ = true;
            }
            sendResponse(req, json::object());
            // What the program loads: the script and clojure.core.
            sourceLoaded(l.program);
            sourceLoaded(embeddedCoreSourceName());
            cv_.notify_all();
            return false;
        }
        if (command == "setBreakpoints") {
            setBreakpoints(req, args);
            return false;
        }
        if (command == "source") {
            std::optional<std::string> adapter;
            const long long ref = args.value("sourceReference", 0LL);
            if (ref > 0) adapter = sourceOfReference(ref);
            else if (args.contains("source")) adapter = adapterSourceOf(args["source"]);
            if (!adapter && args.contains("source")) adapter = adapterSourceOf(args["source"]);
            std::string text;
            if (adapter && sourceText(*adapter, text))
                sendResponse(req, {{"content", text}, {"mimeType", "text/x-clojure"}});
            else
                sendErrorResponse(req, "source: unknown source" + (adapter ? " " + *adapter : std::string()));
            return false;
        }
        if (command == "loadedSources") {
            std::vector<std::string> loaded;
            {
                std::lock_guard<std::mutex> lk(srcMu_);
                loaded = loaded_;
            }
            json list = json::array();
            for (const auto& a : loaded) list.push_back(sourceFor(a));
            sendResponse(req, {{"sources", list}});
            return false;
        }
        if (command == "setExceptionBreakpoints") {
            setExceptionBreakpoints(req, args);
            return false;
        }
        if (command == "configurationDone") {
            sendResponse(req, json::object());
            {
                std::lock_guard<std::mutex> lk(mu_);
                configDone_ = true;
            }
            cv_.notify_all();
            return false;
        }
        if (command == "threads") {
            json list = json::array();
            {
                std::lock_guard<std::mutex> lk(mu_);
                for (const auto& [id, ts] : threads_) list.push_back({{"id", id}, {"name", ts->name}});
            }
            if (list.empty()) list.push_back({{"id", 1}, {"name", "main"}});
            sendResponse(req, {{"threads", list}});
            return false;
        }
        if (command == "continue" || command == "next" || command == "stepIn" || command == "stepOut") {
            const long long threadId = args.value("threadId", 0LL);
            {
                std::lock_guard<std::mutex> lk(mu_);
                ThreadState* ts = threadById(threadId);
                if (ts && ts->stopped && !ts->resumeReady) {
                    ts->resume = command == "continue" ? Resume::Continue
                               : command == "next"     ? Resume::StepOver
                               : command == "stepIn"   ? Resume::StepIn
                                                       : Resume::StepOut;
                    ts->resumeReady = true;
                }
            }
            cv_.notify_all();
            sendResponse(req, command == "continue" ? json{{"allThreadsContinued", false}} : json::object());
            return false;
        }
        if (command == "pause") {
            const long long threadId = args.value("threadId", 0LL);
            bool any = false;
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (ThreadState* ts = threadById(threadId)) {
                    ts->pauseRequested.store(true);
                    any = true;
                } else {
                    for (auto& [id, t] : threads_) { t->pauseRequested.store(true); any = true; }
                }
            }
            if (!any) entryPending_.store(true);   // nothing ran yet: stop main on its first line
            sendResponse(req, json::object());
            return false;
        }
        if (command == "exceptionInfo") {
            json body;
            bool found = false;
            {
                std::lock_guard<std::mutex> lk(mu_);
                ThreadState* ts = threadById(args.value("threadId", 0LL));
                if (ts && ts->stopped && ts->hasException) {
                    const ExceptionStop& ex = ts->exception;
                    body = {{"exceptionId", ex.typeName},
                            {"description", ex.description},
                            {"breakMode", ex.breakMode},
                            {"details", {{"typeName", ex.typeName},
                                         {"message", ex.message},
                                         {"stackTrace", ex.stackTrace}}}};
                    found = true;
                }
            }
            if (found) sendResponse(req, body);
            else sendErrorResponse(req, "exceptionInfo: the thread is not stopped on an exception");
            return false;
        }
        if (command == "stackTrace" || command == "scopes" || command == "variables" ||
            command == "evaluate") {
            long long threadId = 0;
            if (command == "stackTrace") {
                threadId = args.value("threadId", 0LL);
            } else if (command == "variables") {
                threadId = args.value("variablesReference", 0LL) / kIdStride;
            } else {
                const long long frameId = args.value("frameId", 0LL);
                threadId = frameId / kIdStride;
                if (frameId == 0) {
                    // No frame selected (the Debug Console before a stop is
                    // inspected): the thread that stopped last, if it still is.
                    std::lock_guard<std::mutex> lk(mu_);
                    threadId = lastStopped_;
                    ThreadState* ts = threadById(threadId);
                    if (!ts || !ts->stopped) {
                        for (const auto& [id, t] : threads_)
                            if (t->stopped) { threadId = id; break; }
                    }
                }
            }
            if (queueFor(threadId, req)) {
                // The stopped thread answers it.
            } else if (command == "stackTrace") {
                sendResponse(req, {{"stackFrames", json::array()}, {"totalFrames", 0}});
            } else if (command == "evaluate") {
                sendErrorResponse(req, "evaluate: the thread is not stopped");
            } else {
                sendResponse(req, {{command == "scopes" ? "scopes" : "variables", json::array()}});
            }
            return false;
        }
        if (command == "disconnect" || command == "terminate") {
            sendResponse(req, json::object());
            {
                std::lock_guard<std::mutex> lk(mu_);
                disconnected_ = true;
            }
            cv_.notify_all();
            return true;
        }
        // Anything else: nothing to do.
        sendResponse(req, json::object());
        return false;
    }

    void setExceptionBreakpoints(const json& req, const json& args) {
        bool raised = false, uncaught = false;
        std::string raisedCond, uncaughtCond;
        json result = json::array();
        auto take = [&](const std::string& f, const std::string& condition) {
            const bool known = f == "raised" || f == "userUnhandled";
            if (f == "raised") { raised = true; raisedCond = trimmed(condition); }
            if (f == "userUnhandled") { uncaught = true; uncaughtCond = trimmed(condition); }
            if (!known) {
                result.push_back({{"verified", false}, {"message", "unknown filter " + f}});
                return;
            }
            const std::string problem = trimmed(condition).empty() ? "" : syntaxProblem(condition);
            if (!problem.empty()) {
                // Still set: a stop reports the condition's error when it runs.
                const std::string text = "exception breakpoint condition `" + trimmed(condition) +
                                         "` failed: " + problem;
                sendOutput("console", text + "\n");
                result.push_back({{"verified", true}, {"message", text}});
            } else {
                result.push_back({{"verified", true}});
            }
        };
        if (args.contains("filters") && args["filters"].is_array())
            for (const auto& f : args["filters"])
                if (f.is_string()) take(f.get<std::string>(), "");
        if (args.contains("filterOptions") && args["filterOptions"].is_array())
            for (const auto& o : args["filterOptions"])
                if (o.is_object()) take(o.value("filterId", ""), o.value("condition", ""));
        {
            std::lock_guard<std::mutex> lk(mu_);
            exRaisedCondition_ = raisedCond;
            exUncaughtCondition_ = uncaughtCond;
        }
        exRaised_.store(raised);
        exUncaught_.store(uncaught);
        sendResponse(req, {{"breakpoints", result}});
    }

    void setBreakpoints(const json& req, const json& args) {
        const json clientSource = args.value("source", json::object());
        const std::string path = adapterSourceOf(clientSource).value_or("");
        std::string source;
        const bool readable = !path.empty() && sourceText(path, source);
        const std::set<int> candidates = readable ? formLines(source) : std::set<int>{};
        std::map<int, LineBreakpoint> armed;
        json result = json::array();
        if (args.contains("breakpoints") && args["breakpoints"].is_array()) {
            for (const auto& bp : args["breakpoints"]) {
                const int line = bp.value("line", 0);
                // A breakpoint binds to the first line, from its own on, where
                // a form starts.
                auto it = candidates.lower_bound(line);
                if (it == candidates.end()) {
                    result.push_back({{"verified", false}, {"line", line}, {"source", clientSource},
                                      {"message", readable ? "no form starts on or after this line"
                                                           : "the adapter cannot read this source"}});
                    continue;
                }
                LineBreakpoint spec;
                spec.condition = trimmed(bp.value("condition", ""));
                if (bp.contains("logMessage") && bp["logMessage"].is_string()) {
                    spec.logMessage = bp["logMessage"].get<std::string>();
                    spec.isLog = true;
                }
                const std::string hitProblem = spec.hit.parse(bp.value("hitCondition", ""));
                if (!hitProblem.empty()) {
                    result.push_back({{"verified", false}, {"line", *it}, {"source", clientSource},
                                      {"message", hitProblem}});
                    continue;
                }
                json r = {{"verified", true}, {"line", *it}, {"source", clientSource}};
                if (!spec.condition.empty()) {
                    const std::string problem = syntaxProblem(spec.condition);
                    if (!problem.empty()) {
                        // Set anyway: a hit stops and reports the error.
                        const std::string text = "breakpoint condition `" + spec.condition + "` failed: " + problem;
                        sendOutput("console", text + "\n");
                        r["message"] = text;
                    }
                }
                armed[*it] = spec;
                result.push_back(r);
            }
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (armed.empty()) breakpoints_.erase(pathKey(path));
            else breakpoints_[pathKey(path)] = armed;
            anyBreakpoints_.store(!breakpoints_.empty());
            bpGen_.fetch_add(1, std::memory_order_acq_rel);
        }
        sendResponse(req, {{"breakpoints", result}});
    }

    // --- messages -------------------------------------------------------------

    void sendResponse(const json& req, const json& body) {
        json resp = {{"seq", seq_.fetch_add(1)}, {"type", "response"},
                     {"request_seq", req.value("seq", 0)}, {"success", true},
                     {"command", req.value("command", "")}};
        if (!body.is_null()) resp["body"] = body;
        transport_.writeMessage(resp);
    }

    void sendErrorResponse(const json& req, const std::string& message) {
        transport_.writeMessage({{"seq", seq_.fetch_add(1)}, {"type", "response"},
                                 {"request_seq", req.value("seq", 0)}, {"success", false},
                                 {"command", req.value("command", "")}, {"message", message}});
    }

    void sendEvent(const std::string& event, const json& body) {
        json ev = {{"seq", seq_.fetch_add(1)}, {"type", "event"}, {"event", event}};
        if (!body.is_null()) ev["body"] = body;
        transport_.writeMessage(ev);
    }

    void sendOutput(const std::string& category, const std::string& text) {
        sendEvent("output", {{"category", category}, {"output", text}});
    }

    DapStdio         stdio_;          // before transport_: it owns the streams
    DapTransport     transport_;
    std::atomic<int> seq_{1};
    std::thread      reader_;

    // Session state, guarded by mu_ (cv_ signals every change).
    std::mutex              mu_;
    std::condition_variable cv_;
    DapLaunch launch_;
    std::string programKey_;
    bool launched_ = false, configDone_ = false, running_ = false;
    bool inputClosed_ = false, readerDone_ = false, disconnected_ = false;
    std::map<std::string, std::map<int, LineBreakpoint>> breakpoints_;   // pathKey -> line -> breakpoint
    std::string exRaisedCondition_, exUncaughtCondition_;
    std::map<int, std::unique_ptr<ThreadState>> threads_;  // the live threads, by DAP id
    bool mainSeen_ = false;
    int nextThreadId_ = 2;
    int actorWorkers_ = 0;
    int lastStopped_ = 0;

    std::atomic<bool> anyBreakpoints_{false}, entryPending_{false};
    std::atomic<std::uint64_t> bpGen_{1};             // bumped by every setBreakpoints

    // Set by launch, before the program runs; read-only afterwards.
    std::vector<PathMapping> mappings_;
    bool justMyCode_ = true;
    std::string cwd_;

    // Sources (guarded by srcMu_): sourceReference -> adapter-side source,
    // and the sources the program loaded.
    std::mutex srcMu_;
    std::map<std::string, long long> sourceRefs_;
    std::vector<std::string> refSources_;
    std::vector<std::string> loaded_;
    std::atomic<bool> exRaised_{false}, exUncaught_{false};
};

ThreadExit::~ThreadExit() {
    if (!id) return;
    std::lock_guard<std::mutex> lk(serverMu());
    if (gServer) gServer->threadExited(id);
}

} // namespace

int runDapServer(int (*runProgram)(const DapLaunch& launch, void* arg), void* arg) {
    DapServer server;
    return server.run(runProgram, arg);
}

} // namespace protoClojure
