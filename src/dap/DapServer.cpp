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

bool isListValue(const proto::ProtoObject* v) {
    constexpr std::uintptr_t kTagMask = 0x3F, kTagList = 2, kTagListSmall = 25;
    const std::uintptr_t tag = reinterpret_cast<std::uintptr_t>(v) & kTagMask;
    return v != nullptr && (tag == kTagList || tag == kTagListSmall);
}

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
    if (depth > kMaxDepth && (isListValue(v) || isVector(v) || isMap(v) || isSet(ctx, v) ||
                              isSeqObject(ctx, v))) {
        out += "...";
        return;
    }
    if (isListValue(v)) {
        const proto::ProtoList* l = v->asList(ctx);
        const proto::proto_ulong n = l->getSize(ctx);
        out += '(';
        for (proto::proto_ulong i = 0; i < n; ++i) {
            if (i) out += ' ';
            if (i >= kMaxItems) { out += "..."; break; }
            formatInto(ctx, out, l->getAt(ctx, static_cast<int>(i)), depth + 1, realize);
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
            if (isListValue(node) && node->asList(&scope)->getSize(&scope) == 0) break;
            if (!isListValue(node) && !isSeqObject(&scope, node)) break;
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
    if (isListValue(v) || isVector(v)) {
        const proto::ProtoList* l = isVector(v) ? vectorItems(ctx, v) : v->asList(ctx);
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
    if (isListValue(v)) return v->asList(ctx)->getSize(ctx) > 0;
    if (isVector(v)) return vectorItems(ctx, v)->getSize(ctx) > 0;
    if (isMap(v)) return mapCount(ctx, v) > 0;
    if (isSet(ctx, v)) return !setIsEmpty(ctx, v);
    return false;
}

// ---------------------------------------------------------------------------
// The server
// ---------------------------------------------------------------------------

// Set while a stopped thread evaluates an expression for the client: the
// code it runs never stops.
thread_local bool tEvaluating = false;

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

        const char* reason = nullptr;
        if (entry && entryPending_.exchange(false)) {
            reason = "entry";
        } else if (ts->pauseRequested.exchange(false)) {
            reason = "pause";
        } else {
            if (ts->stepKind != StepKind::None) {
                const int depth = depthOf(frame);
                switch (ts->stepKind) {
                    case StepKind::None: break;
                    case StepKind::In:   reason = "step"; break;
                    case StepKind::Over: if (depth <= ts->stepDepth) reason = "step"; break;
                    case StepKind::Out:  if (depth <  ts->stepDepth) reason = "step"; break;
                }
            }
            if (!reason && anyBreakpoints_.load() && breakpointAt(frame)) reason = "breakpoint";
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
        ThreadState* ts = tState;
        if (!ts) ts = registerThread(top);

        // The value stays rooted here while the thread is stopped.
        proto::ProtoContext scope(ctx->space, ctx);
        scope.resizeAutomaticLocals(1);
        scope.setAutomaticLocal(0, value);
        ExceptionStop ex;
        if (raised) {
            ex.breakMode = "always";
        } else if (!caughtByProgram(&scope, top, value)) {
            ex.breakMode = "userUnhandled";
        } else {
            return;
        }
        describeException(&scope, value, site, top, ex);
        stop(&scope, ts, top, "exception", &ex);
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

    bool breakpointAt(const DebugFrame* frame) {
        const BytecodeModule::DebugInfo* di = frame->mod->debugInfo();
        if (!di) return false;
        std::lock_guard<std::mutex> lk(mu_);
        auto it = breakpoints_.find(di->source == launch_.program ? programKey_ : pathKey(di->source));
        return it != breakpoints_.end() && it->second.count(frame->line) > 0;
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
              const ExceptionStop* ex) {
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
                              {"source", {{"name", baseName(di->source)}, {"path", di->source}}}});
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

    // Evaluates `expression` in the selected frame: the frame's locals the
    // expression names are bound to their values, as the parameters of
    // `(fn [locals...] expression)`, compiled and called on this thread.
    void evaluate(proto::ProtoContext* ctx, ThreadState* ts, const json& req, const json& args) {
        std::string expr = args.value("expression", "");
        while (!expr.empty() && std::isspace(static_cast<unsigned char>(expr.back()))) expr.pop_back();
        while (!expr.empty() && std::isspace(static_cast<unsigned char>(expr.front()))) expr.erase(0, 1);
        if (expr.empty()) { sendErrorResponse(req, "nothing to evaluate"); return; }
        DebugFrame* f = frameById(ts, args.value("frameId", 0LL));
        const ActiveCallContext* cc = activeCallContext();
        if (!cc || !cc->engine) { sendErrorResponse(req, "no evaluator on this thread"); return; }

        std::vector<const BytecodeModule::LocalVar*> used;
        if (f) {
            for (const auto* lv : visibleLocals(f)) {
                if (lv->name == expr) {       // a local by itself: its value
                    sendResponse(req, variableBody(ctx, ts, f->ctx->getAutomaticLocal(static_cast<unsigned>(lv->slot)), true));
                    return;
                }
                if (lv->name.find('#') == std::string::npos && lv->name[0] != '&' && mentions(expr, lv->name))
                    used.push_back(lv);
            }
        }
        if (used.size() > 16) { sendErrorResponse(req, "the expression names more than 16 locals"); return; }

        std::string src = "(fn [";
        for (const auto* lv : used) src += lv->name + " ";
        src += "]\n" + expr + "\n)";

        tEvaluating = true;
        struct Reset { ~Reset() { tEvaluating = false; } } reset;
        proto::ProtoContext scope(ctx->space, ctx);
        scope.resizeAutomaticLocals(2);
        std::string error;
        try {
            Reader reader(&scope, src, cc->named);
            reader.setSymbolResolver(&qualifySyntaxQuoteSymbol);
            scope.setAutomaticLocal(0, reader.readOne(&scope));
            scope.setAutomaticLocal(1, evalTopLevel(&scope, scope.getAutomaticLocal(0), nullptr));
            const proto::ProtoObject* argv[16];
            for (std::size_t i = 0; i < used.size(); ++i)
                argv[i] = f->ctx->getAutomaticLocal(static_cast<unsigned>(used[i]->slot));
            scope.setAutomaticLocal(0, cc->engine->invoke(&scope, scope.getAutomaticLocal(1), argv,
                                                          static_cast<unsigned>(used.size())));
            sendResponse(req, variableBody(&scope, ts, scope.getAutomaticLocal(0), false));
            return;
        } catch (const ReaderError& e) {
            error = std::string("read error: ") + e.what();
        } catch (const CompileError& e) {
            error = std::string("compile error: ") + e.what();
        } catch (const proto::ProtoException& e) {
            error = describeThrown(&scope, e.value());
        } catch (const std::exception& e) {
            error = e.what();
        }
        sendErrorResponse(req, error);
    }

    // An evaluate response's body. Only a value that stays alive while the
    // thread is stopped -- a local, or part of one -- can be expanded.
    json variableBody(proto::ProtoContext* ctx, ThreadState* ts, const proto::ProtoObject* v, bool rooted) {
        const long long ref = rooted && hasChildren(ctx, v) ? newRef(ts, {nullptr, v}) : 0;
        return {{"result", formatValue(ctx, v, !rooted)}, {"variablesReference", ref}};
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
            filters.push_back({{"filter", "raised"},
                               {"label", "All Exceptions"},
                               {"description", "Stop at every throw, caught or not."},
                               {"default", false}});
            filters.push_back({{"filter", "userUnhandled"},
                               {"label", "Uncaught Exceptions"},
                               {"description", "Stop at a throw no try/catch of the program catches."},
                               {"default", false}});
            sendResponse(req, {{"supportsConfigurationDoneRequest", true},
                               {"supportsEvaluateForHovers", true},
                               {"supportsTerminateRequest", true},
                               {"supportsExceptionInfoRequest", true},
                               {"exceptionBreakpointFilters", filters}});
            sendEvent("initialized", json::object());
            return false;
        }
        if (command == "launch") {
            DapLaunch l;
            l.program = args.value("program", "");
            l.stopOnEntry = args.value("stopOnEntry", false);
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
            cv_.notify_all();
            return false;
        }
        if (command == "setBreakpoints") {
            setBreakpoints(req, args);
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
        json result = json::array();
        auto take = [&](const std::string& f) {
            const bool known = f == "raised" || f == "userUnhandled";
            if (f == "raised") raised = true;
            if (f == "userUnhandled") uncaught = true;
            result.push_back(known ? json{{"verified", true}}
                                   : json{{"verified", false}, {"message", "unknown filter " + f}});
        };
        if (args.contains("filters") && args["filters"].is_array())
            for (const auto& f : args["filters"])
                if (f.is_string()) take(f.get<std::string>());
        if (args.contains("filterOptions") && args["filterOptions"].is_array())
            for (const auto& o : args["filterOptions"])
                if (o.is_object()) take(o.value("filterId", ""));
        exRaised_.store(raised);
        exUncaught_.store(uncaught);
        sendResponse(req, {{"breakpoints", result}});
    }

    void setBreakpoints(const json& req, const json& args) {
        const std::string path = args.value("source", json::object()).value("path", "");
        std::string source;
        const bool readable = !path.empty() && readFile(path, source);
        const std::set<int> candidates = readable ? formLines(source) : std::set<int>{};
        std::set<int> armed;
        json result = json::array();
        if (args.contains("breakpoints") && args["breakpoints"].is_array()) {
            for (const auto& bp : args["breakpoints"]) {
                const int line = bp.value("line", 0);
                // A breakpoint binds to the first line, from its own on, where
                // a form starts.
                auto it = candidates.lower_bound(line);
                if (it == candidates.end()) {
                    result.push_back({{"verified", false}, {"line", line},
                                      {"message", "no form starts on or after this line"}});
                    continue;
                }
                armed.insert(*it);
                result.push_back({{"verified", true}, {"line", *it}});
            }
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (armed.empty()) breakpoints_.erase(pathKey(path));
            else breakpoints_[pathKey(path)] = armed;
            anyBreakpoints_.store(!breakpoints_.empty());
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
    std::map<std::string, std::set<int>> breakpoints_;   // pathKey -> lines
    std::map<int, std::unique_ptr<ThreadState>> threads_;  // the live threads, by DAP id
    bool mainSeen_ = false;
    int nextThreadId_ = 2;
    int actorWorkers_ = 0;
    int lastStopped_ = 0;

    std::atomic<bool> anyBreakpoints_{false}, entryPending_{false};
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
