#include "StackGuard.h"

#include <pthread.h>
#include <signal.h>

#include <algorithm>
#include <exception>

namespace protoClojure {

namespace detail {
constinit thread_local std::uintptr_t tl_stackLimit = UINTPTR_MAX;
} // namespace detail

namespace {

// Usable stack size of this thread, recorded with its limit for the error
// message.
constinit thread_local std::size_t tl_stackBytes = 0;

// Stack assumed below the first check when the thread library cannot report
// the thread's stack.
constexpr std::size_t kFallbackStackBytes = 512u << 10;

// The lowest usable address and the size of the calling thread's stack.
// Returns false when the platform cannot report them.
bool currentThreadStack(std::uintptr_t* lowest, std::size_t* size) {
#if defined(__APPLE__)
    const pthread_t self = pthread_self();
    const auto top = reinterpret_cast<std::uintptr_t>(pthread_get_stackaddr_np(self));
    *size = pthread_get_stacksize_np(self);
    *lowest = top - *size;
    return *size > 0;
#elif defined(__linux__) || defined(__FreeBSD__)
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) != 0) return false;
    void* address = nullptr;
    std::size_t bytes = 0;
    const bool ok = pthread_attr_getstack(&attr, &address, &bytes) == 0 && bytes > 0;
    pthread_attr_destroy(&attr);
    if (!ok) return false;
    *lowest = reinterpret_cast<std::uintptr_t>(address);
    *size = bytes;
    return true;
#else
    (void)lowest;
    (void)size;
    return false;
#endif
}

std::string describeBytes(std::size_t bytes) {
    if (bytes >= (1u << 20) && bytes % (1u << 20) == 0)
        return std::to_string(bytes >> 20) + " MiB";
    return std::to_string(bytes >> 10) + " KiB";
}

// The default stack size of new threads, or 0 when it cannot be read.
std::size_t defaultThreadStackBytes() {
#if defined(__GLIBC__)
    pthread_attr_t attr;
    if (pthread_getattr_default_np(&attr) != 0) return 0;
    std::size_t bytes = 0;
    pthread_attr_getstacksize(&attr, &bytes);
    pthread_attr_destroy(&attr);
    return bytes;
#else
    return 0;
#endif
}

} // namespace

namespace detail {

void checkNativeStackSlow(std::uintptr_t frameAddress) {
    if (tl_stackLimit == UINTPTR_MAX) {
        std::uintptr_t lowest = 0;
        std::size_t size = 0;
        if (!currentThreadStack(&lowest, &size) || frameAddress < lowest) {
            size = kFallbackStackBytes;
            lowest = frameAddress - size;
        }
        tl_stackBytes = size;
        tl_stackLimit = lowest + std::min(kStackReserveBytes, size / 4);
        if (frameAddress >= tl_stackLimit) return;
    }
    throw StackOverflowError(
        "StackOverflowError: calls or data nested too deeply for the " +
        describeBytes(tl_stackBytes) +
        " thread stack (use loop/recur for deep iteration)");
}

} // namespace detail

void configureThreadStacks() {
#if defined(__GLIBC__)
    pthread_attr_t attr;
    if (pthread_getattr_default_np(&attr) != 0) return;
    std::size_t bytes = 0;
    if (pthread_attr_getstacksize(&attr, &bytes) == 0 && bytes < kThreadStackBytes &&
        pthread_attr_setstacksize(&attr, kThreadStackBytes) == 0) {
        pthread_setattr_default_np(&attr);
    }
    pthread_attr_destroy(&attr);
#endif
}

int runOnEvaluatorThread(int (*body)(void*), void* arg) {
    struct Job {
        int (*body)(void*);
        void* arg;
        int result;
        std::exception_ptr error;
    } job{body, arg, 1, nullptr};

    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) return body(arg);
    const std::size_t bytes = std::max(kThreadStackBytes, defaultThreadStackBytes());
    pthread_t thread;
    const bool created =
        pthread_attr_setstacksize(&attr, bytes) == 0 &&
        pthread_create(&thread, &attr,
            [](void* p) -> void* {
                auto* j = static_cast<Job*>(p);
                try {
                    j->result = j->body(j->arg);
                } catch (...) {
                    j->error = std::current_exception();
                }
                return nullptr;
            },
            &job) == 0;
    pthread_attr_destroy(&attr);
    if (!created) return body(arg);

    sigset_t all;
    sigset_t previous;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &previous);
    pthread_join(thread, nullptr);
    pthread_sigmask(SIG_SETMASK, &previous, nullptr);

    if (job.error) std::rethrow_exception(job.error);
    return job.result;
}

} // namespace protoClojure
