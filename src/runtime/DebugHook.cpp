#include "DebugHook.h"

#include <atomic>

namespace protoClojure {

namespace {
std::atomic<DebugHook*> gHook{nullptr};
thread_local DebugFrame* tTop = nullptr;
thread_local int tQuiet = 0;
thread_local const DebugFrame* tBarrierTop = nullptr;
thread_local bool tBarrierSet = false;
thread_local const char* tThreadKind = nullptr;
} // namespace

void setDebugHook(DebugHook* hook) { gHook.store(hook, std::memory_order_release); }
DebugHook* debugHook() { return gHook.load(std::memory_order_acquire); }

DebugFrame* debugTop() { return tTop; }

void debugNotifyThrow(proto::ProtoContext* ctx, const proto::ProtoObject* value, ThrowSite site) {
    DebugHook* hook = debugHook();
    if (!hook || tQuiet > 0 || !tTop) return;
    try {
        hook->onThrow(ctx, value, site);
    } catch (...) {
        // The throw that follows is the program's; the debugger's own
        // failure is not.
    }
}

DebugQuietScope::DebugQuietScope() { ++tQuiet; }
DebugQuietScope::~DebugQuietScope() { --tQuiet; }
bool debugQuiet() { return tQuiet > 0; }

DebugCatchBarrier::DebugCatchBarrier() : savedTop_(tBarrierTop), savedSet_(tBarrierSet) {
    tBarrierTop = tTop;
    tBarrierSet = true;
}
DebugCatchBarrier::~DebugCatchBarrier() {
    tBarrierTop = savedTop_;
    tBarrierSet = savedSet_;
}
bool debugCatchBarrier(const DebugFrame*& top) {
    top = tBarrierTop;
    return tBarrierSet;
}

void setDebugThreadKind(const char* kind) { tThreadKind = kind; }
const char* debugThreadKind() { return tThreadKind; }

void DebugFrameScope::push() {
    frame_.parent = tTop;
    tTop = &frame_;
}

void DebugFrameScope::pop() {
    // Frames end in the order they began, on an exception too.
    tTop = frame_.parent;
}

} // namespace protoClojure
