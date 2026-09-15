/*
 * StackGuard — native stack checks and the stack size of runtime threads.
 *
 * The bytecode VM runs every non-tail call as a nested
 * ExecutionEngine::execute on the native stack, and the printer, value
 * equality and value hashing recurse natively over nested collections. A
 * recursion deeper than the thread's stack would crash the process with
 * SIGSEGV; checkNativeStack turns it into a StackOverflowError, the analogue
 * of JVM Clojure's java.lang.StackOverflowError, which the script driver,
 * the REPL, future and pmap threads and actor workers handle like any other
 * runtime error.
 *
 * The check compares the address of a local variable with a per-thread
 * limit: the lowest address of the thread's stack, as the thread library
 * reports it (pthread_getattr_np), plus kStackReserveBytes. The limit is
 * computed on the first check a thread makes, so the check needs no
 * per-thread set-up and adapts to any stack size: the evaluator thread,
 * threads created by protoCore, the main thread under any `ulimit -s`. No
 * guard page and no signal handler are involved.
 *
 * Depth. The evaluator (the script driver and the REPL) runs on a thread
 * whose stack holds kThreadStackBytes, and configureThreadStacks raises the
 * default stack size of every thread the process creates later (future and
 * pmap threads, actor workers) to the same size, so a recursion reaches the
 * same depth on every kind of thread. Stack pages are committed only when a
 * recursion touches them.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace protoClojure {

// Raised when a recursion reaches the end of the thread's native stack. The
// message starts with "StackOverflowError:".
class StackOverflowError : public std::runtime_error {
public:
    explicit StackOverflowError(const std::string& message)
        : std::runtime_error(message) {}
};

// Stack size of the evaluator thread and minimum default stack size of the
// threads the process creates after configureThreadStacks.
inline constexpr std::size_t kThreadStackBytes = 32u << 20;

// Stack kept free below the point where checkNativeStack raises: room for
// the work done between two checks (a primitive, protoCore internals, the
// C++ exception machinery unwinding the error). A stack smaller than four
// times this keeps a quarter of its size free instead.
inline constexpr std::size_t kStackReserveBytes = 256u << 10;

namespace detail {
// The lowest stack address a check accepts on this thread; UINTPTR_MAX until
// the thread's first check computes it.
extern constinit thread_local std::uintptr_t tl_stackLimit;

// First check of a thread (computes tl_stackLimit), or an exhausted stack
// (throws StackOverflowError).
[[gnu::cold, gnu::noinline]]
void checkNativeStackSlow(std::uintptr_t frameAddress);
} // namespace detail

// Throws StackOverflowError when the calling function's frame lies within
// kStackReserveBytes of the end of the thread's stack. Cost: one
// thread-local load and one compare.
inline void checkNativeStack() {
    const char probe = 0;
    const auto frameAddress = reinterpret_cast<std::uintptr_t>(&probe);
    if (__builtin_expect(frameAddress < detail::tl_stackLimit, 0))
        detail::checkNativeStackSlow(frameAddress);
}

// Raises the default stack size of threads created from now on without an
// explicit size (std::thread, protoCore's newThread) to kThreadStackBytes,
// unless it is already larger. Call once at start-up, before any thread is
// created. Only effective with glibc; elsewhere threads keep the platform
// default and checkNativeStack still guards them.
void configureThreadStacks();

// Runs `body(arg)` on a new thread whose stack holds kThreadStackBytes (or
// the default stack size, if larger), waits for it and returns its result;
// an exception escaping `body` is rethrown on the calling thread. While
// waiting, the calling thread blocks asynchronous signals, so they are
// delivered to the thread running `body` as they would be to a
// single-threaded program. If the thread cannot be created, `body` runs on
// the calling thread.
int runOnEvaluatorThread(int (*body)(void*), void* arg);

} // namespace protoClojure
