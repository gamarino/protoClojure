// Unit tests for the native stack guard (src/runtime/StackGuard.h). The
// conformance fixtures under tests/conformance/05-recursion, 22-futures and
// 25-actors cover the Clojure-visible behaviour; these tests check the guard
// itself on threads whose stack size the test chooses.

#include "runtime/StackGuard.h"

#include <gtest/gtest.h>

#include <pthread.h>

#include <cstddef>
#include <string>

namespace {

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
};

// Runs `recurse` on a new thread with a `stackBytes` stack.
Probe probeThread(std::size_t stackBytes, StackUse use = StackUse::Evaluation) {
    Probe probe;
    probe.use = use;
    pthread_attr_t attr;
    EXPECT_EQ(pthread_attr_init(&attr), 0);
    EXPECT_EQ(pthread_attr_setstacksize(&attr, stackBytes), 0);
    pthread_t thread;
    const int rc = pthread_create(&thread, &attr,
        [](void* p) -> void* {
            auto* pr = static_cast<Probe*>(p);
            try {
                recurse(pr->depth, pr->use);
            } catch (const StackOverflowError& e) {
                pr->overflowed = true;
                pr->message = e.what();
            }
            return nullptr;
        },
        &probe);
    pthread_attr_destroy(&attr);
    EXPECT_EQ(rc, 0);
    if (rc == 0) pthread_join(thread, nullptr);
    return probe;
}

TEST(StackGuard, ExhaustedStackRaisesStackOverflowError) {
    const Probe p = probeThread(1u << 20);
    EXPECT_TRUE(p.overflowed);
    EXPECT_EQ(p.message.rfind("StackOverflowError:", 0), 0u) << p.message;
    EXPECT_NE(p.message.find("1 MiB"), std::string::npos) << p.message;
    EXPECT_GT(p.depth, 100u);
}

TEST(StackGuard, SourceNestingNamesForms) {
    // The reader and the compiler check with StackUse::Source: the same limit,
    // worded for nested source forms.
    const Probe evaluation = probeThread(1u << 20);
    const Probe source = probeThread(1u << 20, StackUse::Source);
    ASSERT_TRUE(source.overflowed);
    EXPECT_EQ(source.message,
              "StackOverflowError: forms nested too deeply for the 1 MiB thread stack");
    EXPECT_NE(evaluation.message.find("calls or data nested too deeply"),
              std::string::npos) << evaluation.message;
    // Same limit: the two probes stop within a level or two of each other.
    EXPECT_LE(source.depth, evaluation.depth + 2);
    EXPECT_LE(evaluation.depth, source.depth + 2);
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

TEST(StackGuard, EvaluatorThreadHasTheConfiguredStack) {
    std::size_t reported = 0;
    const int result = protoClojure::runOnEvaluatorThread(
        [](void* p) -> int {
            pthread_attr_t attr;
            if (pthread_getattr_np(pthread_self(), &attr) != 0) return -1;
            std::size_t bytes = 0;
            pthread_attr_getstacksize(&attr, &bytes);
            pthread_attr_destroy(&attr);
            *static_cast<std::size_t*>(p) = bytes;
            return 42;
        },
        &reported);
    EXPECT_EQ(result, 42);
    EXPECT_GE(reported, protoClojure::kThreadStackBytes);
}

TEST(StackGuard, EvaluatorThreadRethrowsOnTheCaller) {
    EXPECT_THROW(protoClojure::runOnEvaluatorThread(
                     [](void*) -> int {
                         std::size_t depth = 0;
                         recurse(depth);
                         return 0;
                     },
                     nullptr),
                 StackOverflowError);
}

TEST(StackGuard, ConfiguredDefaultAppliesToNewThreads) {
    protoClojure::configureThreadStacks();
    pthread_attr_t attr;
    ASSERT_EQ(pthread_getattr_default_np(&attr), 0);
    std::size_t bytes = 0;
    pthread_attr_getstacksize(&attr, &bytes);
    pthread_attr_destroy(&attr);
    EXPECT_GE(bytes, protoClojure::kThreadStackBytes);
}

} // namespace
