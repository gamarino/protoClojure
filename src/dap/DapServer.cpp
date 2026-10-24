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
std::vector<std::pair<std::string, const proto::ProtoObject*>>
childrenOf(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
    std::vector<std::pair<std::string, const proto::ProtoObject*>> kids;
    constexpr std::size_t kMaxChildren = 1000;
    if (isListValue(v) || isVector(v)) {
        const proto::ProtoList* l = isVector(v) ? vectorItems(ctx, v) : v->asList(ctx);
        const proto::proto_ulong n = std::min<proto::proto_ulong>(l->getSize(ctx), kMaxChildren);
        for (proto::proto_ulong i = 0; i < n; ++i)
            kids.emplace_back("[" + std::to_string(i) + "]", l->getAt(ctx, static_cast<int>(i)));
    } else if (isMap(v)) {
        struct Acc { proto::ProtoContext* ctx; decltype(kids)* kids; } acc{ctx, &kids};
        mapForEach(ctx, v, &acc, [](proto::ProtoContext* c, void* self,
                                    const proto::ProtoObject* k, const proto::ProtoObject* val) {
            auto* a = static_cast<Acc*>(self);
            if (a->kids->size() < kMaxChildren) a->kids->emplace_back(formatValue(c, k), val);
        });
    } else if (isSet(ctx, v)) {
        struct Acc { decltype(kids)* kids; } acc{&kids};
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

class DapServer final : public DebugHook {
public:
    DapServer() : transport_(stdio_.protocolIn(), stdio_.protocolOut()) {}

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
        if (!anyBreakpoints_.load(std::memory_order_relaxed) &&
            !stepping_.load(std::memory_order_relaxed) &&
            !pauseRequested_.load(std::memory_order_relaxed) &&
            !entryPending_.load(std::memory_order_relaxed))
            return;

        const char* reason = nullptr;
        if (entryPending_.exchange(false)) {
            reason = "entry";
        } else if (pauseRequested_.exchange(false)) {
            reason = "pause";
        } else {
            if (stepping_.load()) {
                std::lock_guard<std::mutex> lk(mu_);
                if (stepping_.load() && std::this_thread::get_id() == stepThread_) {
                    const int depth = depthOf(frame);
                    switch (stepKind_) {
                        case StepKind::In:   reason = "step"; break;
                        case StepKind::Over: if (depth <= stepDepth_) reason = "step"; break;
                        case StepKind::Out:  if (depth <  stepDepth_) reason = "step"; break;
                    }
                }
            }
            if (!reason && anyBreakpoints_.load() && breakpointAt(frame)) reason = "breakpoint";
        }
        if (reason) stop(ctx, frame, reason);
    }

private:
    enum class StepKind { In, Over, Out };
    enum class Resume { Continue, StepIn, StepOver, StepOut };

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

    // The thread stops: tell the client, then answer its inspection requests
    // until it resumes the program. Runs on the stopped thread.
    void stop(proto::ProtoContext* ctx, DebugFrame* frame, const char* reason) {
        // A line printed without a newline is still in stdio's buffer.
        std::fflush(stdout);
        std::fflush(stderr);
        std::cout.flush();
        {
            // One stop at a time; another thread waits here, unmanaged, so a
            // collection never waits for it.
            proto::ProtoContext::UnmanagedScope unmanaged(ctx);
            stopMu_.lock();
            // What the program printed before the stop reaches the client
            // before the `stopped` event.
            stdio_.waitForwarded();
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            stopped_ = true;
            resumeReady_ = false;
            stepping_.store(false);
            stopFrame_ = frame;
            frames_.clear();
            refs_.clear();
        }
        sendEvent("stopped", {{"reason", reason}, {"threadId", 1}, {"allThreadsStopped", true}});

        for (;;) {
            json req;
            bool resume = false;
            {
                proto::ProtoContext::UnmanagedScope unmanaged(ctx);
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [&] { return resumeReady_ || !requests_.empty(); });
                if (!requests_.empty()) {
                    req = std::move(requests_.front());
                    requests_.pop_front();
                } else {
                    resume = true;
                }
            }
            if (resume) break;
            answerStopped(ctx, req);
        }

        std::deque<json> late;
        {
            std::lock_guard<std::mutex> lk(mu_);
            stopped_ = false;
            late.swap(requests_);
            frames_.clear();
            refs_.clear();
            stopFrame_ = nullptr;
            switch (resume_) {
                case Resume::Continue: break;
                case Resume::StepIn:   stepKind_ = StepKind::In;   break;
                case Resume::StepOver: stepKind_ = StepKind::Over; break;
                case Resume::StepOut:  stepKind_ = StepKind::Out;  break;
            }
            if (resume_ != Resume::Continue) {
                stepThread_ = std::this_thread::get_id();
                stepDepth_ = depthOf(frame);
                stepping_.store(true);
            }
        }
        for (const json& r : late) sendErrorResponse(r, "the program is running");
        stopMu_.unlock();
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

    void answerStopped(proto::ProtoContext* ctx, const json& req) {
        const std::string command = req.value("command", "");
        const json args = req.value("arguments", json::object());
        try {
            if (command == "stackTrace")     stackTrace(req, args);
            else if (command == "scopes")    scopes(req, args);
            else if (command == "variables") variables(ctx, req, args);
            else if (command == "evaluate")  evaluate(ctx, req, args);
            else sendResponse(req, json::object());
        } catch (const std::exception& e) {
            sendErrorResponse(req, e.what());
        }
    }

    void stackTrace(const json& req, const json& args) {
        const int start = args.value("startFrame", 0);
        const int levels = args.value("levels", 0);
        std::vector<DebugFrame*> stack;
        for (DebugFrame* f = stopFrame_; f; f = f->parent) {
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
            const int id = static_cast<int>(frames_.size()) + 1;
            frames_.push_back(f);
            const BytecodeModule::DebugInfo* di = f->mod->debugInfo();
            frames.push_back({{"id", id},
                              {"name", di->name},
                              {"line", f->shownLine},
                              {"column", 1},
                              {"source", {{"name", baseName(di->source)}, {"path", di->source}}}});
        }
        sendResponse(req, {{"stackFrames", frames}, {"totalFrames", total}});
    }

    DebugFrame* frameById(int id) {
        if (id >= 1 && static_cast<std::size_t>(id) <= frames_.size()) return frames_[static_cast<std::size_t>(id) - 1];
        return stopFrame_;
    }

    struct Ref {
        DebugFrame* frame;                   // the Locals of this frame...
        const proto::ProtoObject* value;     // ...or this value's children
    };

    int newRef(Ref r) {
        refs_.push_back(r);
        return static_cast<int>(refs_.size());
    }

    void scopes(const json& req, const json& args) {
        DebugFrame* f = frameById(args.value("frameId", 0));
        json list = json::array();
        if (f) {
            list.push_back({{"name", "Locals"},
                            {"presentationHint", "locals"},
                            {"variablesReference", newRef({f, nullptr})},
                            {"expensive", false}});
        }
        sendResponse(req, {{"scopes", list}});
    }

    json variable(proto::ProtoContext* ctx, const std::string& name, const proto::ProtoObject* v) {
        // A value reachable from a stopped frame's local stays alive while
        // the thread is stopped, so its children can be listed later.
        const int ref = hasChildren(ctx, v) ? newRef({nullptr, v}) : 0;
        return {{"name", name}, {"value", formatValue(ctx, v)}, {"variablesReference", ref}};
    }

    void variables(proto::ProtoContext* ctx, const json& req, const json& args) {
        const int ref = args.value("variablesReference", 0);
        json list = json::array();
        if (ref >= 1 && static_cast<std::size_t>(ref) <= refs_.size()) {
            const Ref r = refs_[static_cast<std::size_t>(ref) - 1];
            if (r.frame) {
                for (const auto* lv : visibleLocals(r.frame))
                    list.push_back(variable(ctx, lv->name,
                                            r.frame->ctx->getAutomaticLocal(static_cast<unsigned>(lv->slot))));
            } else if (r.value) {
                for (const auto& [name, v] : childrenOf(ctx, r.value)) list.push_back(variable(ctx, name, v));
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
    void evaluate(proto::ProtoContext* ctx, const json& req, const json& args) {
        std::string expr = args.value("expression", "");
        while (!expr.empty() && std::isspace(static_cast<unsigned char>(expr.back()))) expr.pop_back();
        while (!expr.empty() && std::isspace(static_cast<unsigned char>(expr.front()))) expr.erase(0, 1);
        if (expr.empty()) { sendErrorResponse(req, "nothing to evaluate"); return; }
        DebugFrame* f = frameById(args.value("frameId", 0));
        const ActiveCallContext* cc = activeCallContext();
        if (!cc || !cc->engine) { sendErrorResponse(req, "no evaluator on this thread"); return; }

        std::vector<const BytecodeModule::LocalVar*> used;
        if (f) {
            for (const auto* lv : visibleLocals(f)) {
                if (lv->name == expr) {       // a local by itself: its value
                    sendResponse(req, variableBody(ctx, f->ctx->getAutomaticLocal(static_cast<unsigned>(lv->slot)), true));
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
            sendResponse(req, variableBody(&scope, scope.getAutomaticLocal(0), false));
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
    json variableBody(proto::ProtoContext* ctx, const proto::ProtoObject* v, bool rooted) {
        const int ref = rooted && hasChildren(ctx, v) ? newRef({nullptr, v}) : 0;
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

    // Answers true when the session is over.
    bool handle(const json& req) {
        const std::string command = req.value("command", "");
        const json args = req.value("arguments", json::object());

        if (command == "initialize") {
            sendResponse(req, {{"supportsConfigurationDoneRequest", true},
                               {"supportsEvaluateForHovers", true},
                               {"supportsTerminateRequest", true}});
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
            sendResponse(req, {{"threads", json::array({{{"id", 1}, {"name", "main"}}})}});
            return false;
        }
        if (command == "continue" || command == "next" || command == "stepIn" || command == "stepOut") {
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (stopped_ && !resumeReady_) {
                    resume_ = command == "continue" ? Resume::Continue
                            : command == "next"     ? Resume::StepOver
                            : command == "stepIn"   ? Resume::StepIn
                                                    : Resume::StepOut;
                    resumeReady_ = true;
                }
            }
            cv_.notify_all();
            sendResponse(req, command == "continue" ? json{{"allThreadsContinued", true}} : json::object());
            return false;
        }
        if (command == "pause") {
            pauseRequested_.store(true);
            sendResponse(req, json::object());
            return false;
        }
        if (command == "stackTrace" || command == "scopes" || command == "variables" ||
            command == "evaluate") {
            bool queued = false;
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (stopped_ && !resumeReady_) {
                    requests_.push_back(req);
                    queued = true;
                }
            }
            if (queued) {
                cv_.notify_all();
            } else if (command == "stackTrace") {
                sendResponse(req, {{"stackFrames", json::array()}, {"totalFrames", 0}});
            } else if (command == "evaluate") {
                sendErrorResponse(req, "evaluate: the program is not stopped");
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
        // Anything else (setExceptionBreakpoints, ...): nothing to do.
        sendResponse(req, json::object());
        return false;
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
    bool stopped_ = false, resumeReady_ = false;
    Resume resume_ = Resume::Continue;
    std::deque<json> requests_;                          // for the stopped thread
    StepKind stepKind_ = StepKind::In;
    std::thread::id stepThread_;
    int stepDepth_ = 0;

    std::atomic<bool> anyBreakpoints_{false}, stepping_{false};
    std::atomic<bool> pauseRequested_{false}, entryPending_{false};

    // Owned by the stopped thread (stopMu_ held) while it is stopped.
    std::mutex                stopMu_;
    DebugFrame*               stopFrame_ = nullptr;
    std::vector<DebugFrame*>  frames_;   // frameId - 1 -> frame
    std::vector<Ref>          refs_;     // variablesReference - 1 -> what
};

} // namespace

int runDapServer(int (*runProgram)(const DapLaunch& launch, void* arg), void* arg) {
    DapServer server;
    return server.run(runProgram, arg);
}

} // namespace protoClojure
