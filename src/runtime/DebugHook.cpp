#include "DebugHook.h"

#include <atomic>

namespace protoClojure {

namespace {
std::atomic<DebugHook*> gHook{nullptr};
thread_local DebugFrame* tTop = nullptr;
} // namespace

void setDebugHook(DebugHook* hook) { gHook.store(hook, std::memory_order_release); }
DebugHook* debugHook() { return gHook.load(std::memory_order_acquire); }

DebugFrame* debugTop() { return tTop; }

void DebugFrameScope::push() {
    frame_.parent = tTop;
    tTop = &frame_;
    active_ = true;
}

void DebugFrameScope::pop() {
    // Frames end in the order they began, on an exception too.
    tTop = frame_.parent;
    active_ = false;
}

} // namespace protoClojure
