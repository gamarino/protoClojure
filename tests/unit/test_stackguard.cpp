// Unit tests for the native stack guard (src/runtime/StackGuard.h). The
// conformance fixtures under tests/conformance/05-recursion, 22-futures and
// 25-actors cover the Clojure-visible behaviour; these tests check the guard
// itself on threads whose stack size the test chooses.

#include "runtime/StackGuard.h"

#include "protoCore.h"

#include <gtest/gtest.h>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <process.h>
// rpcndr.h, included by windows.h, defines `small` as `char`.
#undef small
#else
#include <pthread.h>
#endif

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

// The calling thread's stack: its lowest and highest address, as the
// platform reports them.
void stackBounds(std::uintptr_t& low, std::uintptr_t& high) {
#if defined(_WIN32)
    ULONG_PTR l = 0, h = 0;
    GetCurrentThreadStackLimits(&l, &h);
    low = static_cast<std::uintptr_t>(l);
    high = static_cast<std::uintptr_t>(h);
#elif defined(__APPLE__)
    high = reinterpret_cast<std::uintptr_t>(pthread_get_stackaddr_np(pthread_self()));
    low = high - pthread_get_stacksize_np(pthread_self());
#else
    pthread_attr_t attr;
    void* address = nullptr;
    std::size_t bytes = 0;
    low = high = 0;
    if (pthread_getattr_np(pthread_self(), &attr) != 0) return;
    pthread_attr_getstack(&attr, &address, &bytes);
    pthread_attr_destroy(&attr);
    low = reinterpret_cast<std::uintptr_t>(address);
    high = low + bytes;
#endif
}

// The calling thread's stack size.
std::size_t currentStackBytes() {
    std::uintptr_t low = 0, high = 0;
    stackBounds(low, high);
    return static_cast<std::size_t>(high - low);
}

using protoClojure::StackOverflowError;

using protoClojure::StackUse;

// Recurses until checkNativeStack throws, counting the levels in `depth`.
// Each level keeps a buffer alive across the call, so the recursion uses at
// least 512 bytes of stack per level and cannot become a loop. The depth
// bound is beyond any stack the tests create (it would take 512 GiB).
[[gnu::noinline]]
void recurse(std::size_t& depth, StackUse use = StackUse::Evaluation) {
    protoClojure::checkNativeStack(use);
    volatile char pad[512];
    pad[0] = 1;
    if (++depth > (std::size_t{1} << 30)) return;
    recurse(depth, use);
    pad[1] = pad[0];
}

struct Probe {
    std::size_t depth = 0;
    bool overflowed = false;
    std::string message;
    StackUse use = StackUse::Evaluation;
    // How far below the top of its stack the thread's first frame lies.
    std::size_t entryOffset = 0;
};

// The body of a probe thread.
void runProbe(Probe* pr) {
    std::uintptr_t low = 0, high = 0;
    stackBounds(low, high);
    const char here = 0;
    pr->entryOffset = static_cast<std::size_t>(high - reinterpret_cast<std::uintptr_t>(&here));
    try {
        recurse(pr->depth, pr->use);
    } catch (const StackOverflowError& e) {
        pr->overflowed = true;
        pr->message = e.what();
    }
}

// Runs `recurse` on a new thread with a `stackBytes` stack.
Probe probeThread(std::size_t stackBytes, StackUse use = StackUse::Evaluation) {
    Probe probe;
    probe.use = use;
#if defined(_WIN32)
    // The stack is reserved with exactly `stackBytes`, as pthread_attr_setstacksize does.
    const auto handle = reinterpret_cast<HANDLE>(_beginthreadex(
        nullptr, static_cast<unsigned>(stackBytes),
        [](void* p) -> unsigned {
            runProbe(static_cast<Probe*>(p));
            return 0;
        },
        &probe, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr));
    EXPECT_NE(handle, nullptr);
    if (handle) {
        WaitForSingleObject(handle, INFINITE);
        CloseHandle(handle);
    }
    return probe;
#else
    pthread_attr_t attr;
    EXPECT_EQ(pthread_attr_init(&attr), 0);
    EXPECT_EQ(pthread_attr_setstacksize(&attr, stackBytes), 0);
    pthread_t thread;
    const int rc = pthread_create(&thread, &attr,
        [](void* p) -> void* {
            runProbe(static_cast<Probe*>(p));
            return nullptr;
        },
        &probe);
    pthread_attr_destroy(&attr);
    EXPECT_EQ(rc, 0);
    if (rc == 0) pthread_join(thread, nullptr);
    return probe;
#endif
}

TEST(StackGuard, ExhaustedStackRaisesStackOverflowError) {
    const Probe p = probeThread(1u << 20);
    EXPECT_TRUE(p.overflowed);
    EXPECT_EQ(p.message.rfind("StackOverflowError:", 0), 0u) << p.message;
#if defined(__APPLE__)
    // macOS reports the size with its guard pages (1036 KiB for 1 MiB).
    EXPECT_NE(p.message.find(" KiB thread stack"), std::string::npos) << p.message;
#else
    EXPECT_NE(p.message.find("1 MiB"), std::string::npos) << p.message;
#endif
    EXPECT_GT(p.depth, 100u);
}

TEST(StackGuard, SourceNestingNamesForms) {
    // The reader and the compiler check with StackUse::Source: the same limit,
    // worded for nested source forms.
    const Probe evaluation = probeThread(1u << 20);
    const Probe source = probeThread(1u << 20, StackUse::Source);
    ASSERT_TRUE(source.overflowed);
#if defined(__APPLE__)
    // macOS reports the size with its guard pages (1036 KiB for 1 MiB).
    EXPECT_EQ(source.message.rfind("StackOverflowError: forms nested too deeply for the ", 0), 0u)
        << source.message;
#else
    EXPECT_EQ(source.message,
              "StackOverflowError: forms nested too deeply for the 1 MiB thread stack");
#endif
    EXPECT_NE(evaluation.message.find("calls or data nested too deeply"),
              std::string::npos) << evaluation.message;
    // The measurement behind the slack below, printed so CI logs carry it.
    std::printf("probe entry offsets below the stack top: evaluation %zu, source %zu bytes; "
                "depths %zu and %zu\n",
                evaluation.entryOffset, source.entryOffset, evaluation.depth, source.depth);
    // Same limit: the two probes stop within a level or two of each other.
    // On Windows each new thread starts a varying distance into its stack
    // reservation, so two probe threads differ by a few levels more.
#if defined(_WIN32)
    constexpr std::size_t kSlack = 8;
#else
    constexpr std::size_t kSlack = 2;
#endif
    EXPECT_LE(source.depth, evaluation.depth + kSlack);
    EXPECT_LE(evaluation.depth, source.depth + kSlack);
}

TEST(StackGuard, LimitFollowsTheThreadStackSize) {
    const Probe small = probeThread(1u << 20);
    const Probe large = probeThread(16u << 20);
    ASSERT_TRUE(small.overflowed);
    ASSERT_TRUE(large.overflowed);
    // 16 MiB less the 256 KiB reserve holds 21 times the 768 KiB a 1 MiB
    // stack keeps usable.
    EXPECT_GT(large.depth, small.depth * 15);
}

TEST(StackGuard, MainThreadIsGuarded) {
    std::size_t depth = 0;
    EXPECT_THROW(recurse(depth), StackOverflowError);
    // The main thread can be checked again after an overflow.
    depth = 0;
    EXPECT_THROW(recurse(depth), StackOverflowError);
}

// The evaluator is a protoCore thread with the configured stack.
TEST(StackGuard, EvaluatorThreadHasTheConfiguredStack) {
    protoClojure::configureThreadStacks();
    proto::ProtoSpace space;
    struct Seen {
        std::size_t bytes = 0;
        bool protoThread = false;
    } seen;
    const int result = protoClojure::runOnEvaluatorThread(
        space,
        [](proto::ProtoContext* ctx, void* p) -> int {
            auto* s = static_cast<Seen*>(p);
            s->bytes = currentStackBytes();
            s->protoThread = ctx->thread != nullptr && ctx->thread != ctx->space->rootContext->thread;
            return 42;
        },
        &seen);
    EXPECT_EQ(result, 42);
    EXPECT_TRUE(seen.protoThread);
    EXPECT_GE(seen.bytes, protoClojure::kThreadStackBytes);
}

TEST(StackGuard, EvaluatorThreadRethrowsOnTheCaller) {
    protoClojure::configureThreadStacks();
    proto::ProtoSpace space;
    EXPECT_THROW(protoClojure::runOnEvaluatorThread(
                     space,
                     [](proto::ProtoContext*, void*) -> int {
                         std::size_t depth = 0;
                         recurse(depth);
                         return 0;
                     },
                     nullptr),
                 StackOverflowError);
}

// The threads protoCore creates -- future, pmap and actor worker threads --
// get kThreadStackBytes once configureThreadStacks ran: through glibc's
// default thread attribute on Linux, ProtoSpace::setThreadStackBytes on
// macOS, and on Windows the executable's /STACK reservation (this test
// program is linked with the same /STACK as protoclj.exe).
std::size_t g_protoThreadStackBytes = 0;

const proto::ProtoObject* reportStack(proto::ProtoContext*, const proto::ProtoObject*,
                                      const proto::ParentLink*, const proto::ProtoList*,
                                      const proto::ProtoSparseList*) {
    g_protoThreadStackBytes = currentStackBytes();
    return PROTO_NONE;
}

TEST(StackGuard, ConfiguredDefaultAppliesToNewThreads) {
    protoClojure::configureThreadStacks();
    proto::ProtoSpace space;
    proto::ProtoContext* ctx = space.rootContext;
    const proto::ProtoThread* thread = space.newThread(
        ctx, proto::ProtoString::createSymbol(ctx, "stack-probe"), &reportStack, ctx->newList(),
        nullptr);
    ASSERT_NE(thread, nullptr);
    const_cast<proto::ProtoThread*>(thread)->join(ctx);
    EXPECT_GE(g_protoThreadStackBytes, protoClojure::kThreadStackBytes);
}

} // namespace
