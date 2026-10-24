/*
 * DebugHook — what the VM tells a debugger (docs/debugging.md).
 *
 * Code compiled for the debugger (`protoclj --dap`, SourceInfo.h) carries a
 * DebugInfo (BytecodeModule.h) and line marks (Op::LINE). A frame that runs
 * such a body keeps a DebugFrame on its thread's chain for as long as it
 * runs, and every line mark that ENTERS a line (one other than the line the
 * frame is on) calls the installed hook on the running thread, with the
 * frame's own context as the innermost one. The hook may stop the thread
 * there: while it does, the frames of the chain are alive and their locals
 * are slots of their contexts (`ctx`), which the collector traces.
 *
 * The runtime's throw funnels -- THROW, the runtime's raise helpers
 * (throwClassed, throwError) and the space's error factory protoCore's
 * kernel errors go through (Exceptions.cpp) -- call the hook's onThrow
 * BEFORE the C++ exception is thrown, so a debugger can stop with the stack
 * of the throw intact. A rethrow (RETHROW, a finally's or an unmatched
 * catch's) and a C++ exception that is not a ProtoException (a module
 * defect, StackOverflowError) do not call it.
 *
 * Code without debug information (core.clj, everything outside --dap) has
 * no frame on the chain and no line marks; such a frame costs one test of
 * its module's DebugInfo pointer. Without a hook a throw costs one load.
 */
#pragma once

#include "BytecodeModule.h"

#include <cstddef>

namespace proto {
class ProtoContext;
class ProtoObject;
}

namespace protoClojure {

struct DebugFrame {
    DebugFrame*           parent = nullptr;  // the caller's (debug) frame, or nullptr
    const BytecodeModule* mod = nullptr;     // has a DebugInfo
    proto::ProtoContext*  ctx = nullptr;     // the frame's context: locals are its slots
    std::size_t           pc = 0;            // the last line mark the frame passed
    int                   line = 0;          // the line it is on; 0 after a loop's back-edge
    int                   shownLine = 0;     // the line it was last on (for the call stack)
    const void*           handlers = nullptr;  // its active try handlers (debugFrameCatches)
};

// Where a thrown value comes from (DebugHook::onThrow).
enum class ThrowSite {
    Throw,     // the program's `throw`
    Runtime,   // the runtime on the program's behalf (a primitive, a VM check)
    Kernel,    // protoCore (division by zero, overflow...), via the error factory
};

class DebugHook {
public:
    virtual ~DebugHook() = default;
    // `frame` entered `frame->line`. Runs on the frame's thread; `ctx` is the
    // frame's context, the innermost of the thread.
    virtual void onLine(proto::ProtoContext* ctx, DebugFrame* frame) = 0;
    // `value` (rooted by the caller) is about to be thrown on this thread;
    // `ctx` is the raising context, the innermost of the thread. Must not
    // throw.
    virtual void onThrow(proto::ProtoContext* ctx, const proto::ProtoObject* value,
                         ThrowSite site) = 0;
};

// The installed hook (nullptr: none). Set once, before the program runs.
void setDebugHook(DebugHook* hook);
DebugHook* debugHook();

// The calling thread's innermost DebugFrame, or nullptr.
DebugFrame* debugTop();

// The throw funnels' call (Exceptions.cpp): tells the hook, if one is
// installed and the thread is not in a DebugQuietScope. Never throws.
void debugNotifyThrow(proto::ProtoContext* ctx, const proto::ProtoObject* value, ThrowSite site);

// A region whose raises are the runtime's own probes, caught by the C++
// that makes them (an integer that may not fit a long): no exception stop.
class DebugQuietScope {
public:
    DebugQuietScope();
    ~DebugQuietScope();
    DebugQuietScope(const DebugQuietScope&) = delete;
    DebugQuietScope& operator=(const DebugQuietScope&) = delete;
};
bool debugQuiet();

// A native call site that catches whatever the code it calls throws (an
// atom's watch): frames from the thread's innermost DebugFrame at the time
// of construction on are outside, for the question "does the program catch
// this?" (DapServer's userUnhandled filter).
class DebugCatchBarrier {
public:
    DebugCatchBarrier();
    ~DebugCatchBarrier();
    DebugCatchBarrier(const DebugCatchBarrier&) = delete;
    DebugCatchBarrier& operator=(const DebugCatchBarrier&) = delete;

private:
    const DebugFrame* savedTop_;
    bool              savedSet_;
};
// Whether a barrier is active, and the frame it sits on (nullptr: below
// every frame of the thread).
bool debugCatchBarrier(const DebugFrame*& top);

// Whether one of the try handlers active in `f` catches `value`: one of its
// catch clauses selects the value (a finally alone does not). A clause whose
// type cannot be worked out without running code counts as catching.
// ExecutionEngine.cpp. Runs on f's thread, in a DebugQuietScope of its own.
bool debugFrameCatches(proto::ProtoContext* ctx, const DebugFrame* f,
                       const proto::ProtoObject* value);

// What the calling thread is, for the debugger's thread names: "future",
// "pmap", "actor", "http"; nullptr for the evaluator thread. Set by the
// runtime's thread entry points.
void setDebugThreadKind(const char* kind);
const char* debugThreadKind();

// Pushes a DebugFrame for a body with debug information, for the frame's
// lifetime (ExecutionEngine::executeFrame).
class DebugFrameScope {
public:
    DebugFrameScope(const BytecodeModule& mod, proto::ProtoContext* ctx) {
        if (mod.debugInfo()) [[unlikely]] {
            frame_.mod = &mod;
            frame_.ctx = ctx;
            push();
        }
    }
    ~DebugFrameScope() { if (active()) pop(); }
    DebugFrameScope(const DebugFrameScope&) = delete;
    DebugFrameScope& operator=(const DebugFrameScope&) = delete;

    // No flag of its own: the scope is as large as a DebugFrame, which keeps
    // executeFrame's native frame at its size.
    bool active() const { return frame_.mod != nullptr; }
    DebugFrame& frame() { return frame_; }

private:
    void push();
    void pop();
    DebugFrame frame_;
};

} // namespace protoClojure
