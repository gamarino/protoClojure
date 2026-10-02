#include "StackGuard.h"

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <pthread.h>
#include <signal.h>
#endif

#include "protoCore.h"

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
#elif defined(_WIN32)
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    *lowest = static_cast<std::uintptr_t>(low);
    *size = static_cast<std::size_t>(high - low);
    return *size > 0;
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

} // namespace

namespace detail {

void checkNativeStackSlow(std::uintptr_t frameAddress, StackUse use) {
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
    if (use == StackUse::Source) {
        throw StackOverflowError(
            "StackOverflowError: forms nested too deeply for the " +
            describeBytes(tl_stackBytes) + " thread stack");
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
#if defined(PROTOCORE_HAS_THREAD_STACK_BYTES)
    // The threads protoCore creates (the evaluator, futures, pmap, actor
    // workers) get this size from protoCore itself: on macOS since protoCore
    // 2.8.0 (macOS has no process-wide default; a secondary thread gets
    // 512 KiB), on Linux and Windows since 2.9.0. With an older protoCore
    // they get it from glibc's default above and, on Windows, from the
    // executable's /STACK reservation (CMakeLists.txt).
    proto::ProtoSpace::setThreadStackBytes(kThreadStackBytes);
#endif
}

namespace {

struct EvaluatorJob {
    int (*body)(proto::ProtoContext*, void*);
    void* arg;
    int result;
    std::exception_ptr error;
};

// The main of the evaluator thread: args[0] is the EvaluatorJob, as a long.
const proto::ProtoObject* evaluatorMain(proto::ProtoContext* ctx, const proto::ProtoObject*,
                                        const proto::ParentLink*, const proto::ProtoList* args,
                                        const proto::ProtoSparseList*) {
    auto* job = reinterpret_cast<EvaluatorJob*>(args->getAt(ctx, 0)->asLong(ctx));
    try {
        job->result = job->body(ctx, job->arg);
    } catch (...) {
        job->error = std::current_exception();
    }
    return PROTO_NONE;
}

} // namespace

int runOnEvaluatorThread(proto::ProtoSpace& space, int (*body)(proto::ProtoContext*, void*),
                         void* arg) {
    EvaluatorJob job{body, arg, 1, nullptr};
    proto::ProtoContext* ctx = space.rootContext;
    const proto::ProtoThread* thread = nullptr;
    {
        // The argument list is rooted by the new thread; this context hands
        // the cells made here to the collector.
        proto::ProtoContext spawn(&space, ctx);
        const proto::ProtoList* args =
            spawn.newList()->appendLast(&spawn, spawn.fromLong(reinterpret_cast<long long>(&job)));
        try {
            thread = space.newThread(&spawn, proto::ProtoString::createSymbol(&spawn, "protoclj-evaluator"),
                                     &evaluatorMain, args, nullptr);
        } catch (const std::exception&) {
            thread = nullptr;
        }
    }
    if (!thread) return body(ctx, arg);

#if !defined(_WIN32)
    // While waiting, this thread blocks asynchronous signals, so they are
    // delivered to the evaluator as they would be to a single-threaded
    // program. (Windows has no asynchronous signals: the console's Ctrl+C
    // handler runs on a thread of its own.)
    sigset_t all;
    sigset_t previous;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &previous);
#endif
    const_cast<proto::ProtoThread*>(thread)->join(ctx);
#if !defined(_WIN32)
    pthread_sigmask(SIG_SETMASK, &previous, nullptr);
#endif

    if (job.error) std::rethrow_exception(job.error);
    return job.result;
}

} // namespace protoClojure
