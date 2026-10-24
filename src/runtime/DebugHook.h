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
 * Code without debug information (core.clj, everything outside --dap) has
 * no frame on the chain and no line marks; such a frame costs one test of
 * its module's DebugInfo pointer.
 */
#pragma once

#include "BytecodeModule.h"

#include <cstddef>

namespace proto {
class ProtoContext;
}

namespace protoClojure {

struct DebugFrame {
    DebugFrame*           parent = nullptr;  // the caller's (debug) frame, or nullptr
    const BytecodeModule* mod = nullptr;     // has a DebugInfo
    proto::ProtoContext*  ctx = nullptr;     // the frame's context: locals are its slots
    std::size_t           pc = 0;            // the last line mark the frame passed
    int                   line = 0;          // the line it is on; 0 after a loop's back-edge
    int                   shownLine = 0;     // the line it was last on (for the call stack)
};

class DebugHook {
public:
    virtual ~DebugHook() = default;
    // `frame` entered `frame->line`. Runs on the frame's thread; `ctx` is the
    // frame's context, the innermost of the thread.
    virtual void onLine(proto::ProtoContext* ctx, DebugFrame* frame) = 0;
};

// The installed hook (nullptr: none). Set once, before the program runs.
void setDebugHook(DebugHook* hook);
DebugHook* debugHook();

// The calling thread's innermost DebugFrame, or nullptr.
DebugFrame* debugTop();

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
    ~DebugFrameScope() { if (active_) pop(); }
    DebugFrameScope(const DebugFrameScope&) = delete;
    DebugFrameScope& operator=(const DebugFrameScope&) = delete;

    bool active() const { return active_; }
    DebugFrame& frame() { return frame_; }

private:
    void push();
    void pop();
    DebugFrame frame_;
    bool       active_ = false;
};

} // namespace protoClojure
