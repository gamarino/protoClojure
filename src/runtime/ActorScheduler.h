/*
 * ActorScheduler — global worker pool for protoClojure actors.
 *
 * Three priority queues (High / Medium / Low) of READY actors;
 * `N` worker threads drain the highest-non-empty queue first. Each
 * actor has its own mailbox, drained one message at a time (the
 * single-method invariant — same actor never runs concurrently with
 * itself). Workers are protoCore threads (`ProtoSpace::newThread`)
 * so they participate in the GC quorum.
 *
 * Per-actor mailbox: one protoCore `ProtoMPSCQueue` per priority band
 * (PMQ-SPEC). Senders push lock-free and in O(1) from any thread; the
 * running worker drains a whole band with one `takeAll`, which returns
 * the batch in FIFO order as an immutable `ProtoList`. A `claimed`
 * atomic bool replaces the old `running`/`scheduled` booleans and
 * per-actor std::mutex: it is the single source of truth for "this
 * actor is being handled (queued or running) — do not re-enqueue".
 *
 * The three queues are held as an attribute of the actor's wrapper
 * object, which is mutable and therefore a GC root, so the queues —
 * and every message in them, with its function, its arguments and its
 * promise — are reachable by the collector. They used to be
 * `std::atomic<ActorMessage*>` stacks of C++ heap nodes holding
 * unrooted protoCore pointers.
 *
 * Global ready queues remain mutex-protected — only touched once per
 * actor activation, not once per message.
 *
 * Worker count is configured via the `PROTOCLJ_ACTOR_WORKERS` env
 * var. Default: `max(2, hardware_concurrency() - 2)`, capped at 16.
 *
 * The scheduler is a process-wide singleton initialised lazily on
 * the first `actor` allocation and shut down explicitly before the
 * ProtoSpace destructs (in main.cpp).
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace proto {
class ProtoContext;
class ProtoObject;
class ProtoSpace;
class ProtoList;
class ProtoMPSCQueue;
class ProtoThread;
}

namespace protoClojure {

struct ActiveCallContext;

enum class ActorPriority {
    High   = 0,
    Medium = 1,
    Low    = 2,
};

// One enqueued send, as a three-element ProtoList so the collector
// traces it: the function, its extra arguments (a ProtoList, or nil)
// and the promise to deliver the result into (or nil). Built by
// newMessage below and read back with these indices.
namespace message {
constexpr int kFn      = 0;
constexpr int kArgs    = 1;
constexpr int kPromise = 2;
constexpr int kSize    = 3;
}  // namespace message

// The message list for one send. Allocated in `ctx`; the caller pushes
// it onto a mailbox, which is what roots it from then on.
const proto::ProtoObject* newMessage(proto::ProtoContext* ctx,
                                     const proto::ProtoObject* fn,
                                     const proto::ProtoObject* args,
                                     const proto::ProtoObject* promise);

// One actor's runtime state. Pinned by the wrapper ProtoObject's
// `__actor_state__` attribute (long pointer). The scheduler owns
// these via unique_ptr; they live for the process lifetime.
//
// Lock-free model:
//   * Senders push onto `mailbox[band]`, protoCore's lock-free
//     multi-producer queue. No mutex, no allocator contention on the
//     actor itself.
//   * The running worker (guaranteed unique by `claimed`) drains each
//     band in turn with one `takeAll`, which hands back that band's
//     messages in FIFO order, and processes them.
//   * `claimed` toggles 0→1 when an actor enters the ready queue
//     (whether the trigger is a sender or the running worker at
//     end-of-batch) and 1→0 when a worker finishes its batch with
//     nothing more to do. Senders that observe `claimed == 1` skip
//     the schedule path — the running/queued worker will see their
//     message at end-of-batch.
struct ActorState {
    // Rooted through the wrapper's __mailbox__ attribute, never through
    // these raw pointers.
    const proto::ProtoMPSCQueue* mailbox[3]{nullptr, nullptr, nullptr};
    std::atomic<bool>            claimed{false};
    const proto::ProtoObject*    value      = nullptr;  // mirrored under wrapper __value__
    const proto::ProtoObject*    wrapper    = nullptr;
};

class ActorScheduler {
public:
    static ActorScheduler& instance();

    // Idempotent. Called from every `actor` allocation; the first
    // call snapshots the cc into a static buffer and spawns workers.
    void ensureStarted(proto::ProtoSpace* space,
                       proto::ProtoContext* mainCtx,
                       const ActiveCallContext& cc);

    // Join all workers, after draining. Called from main.cpp before
    // the ProtoSpace destructs.
    void shutdown(proto::ProtoContext* ctx);

    // The three queues must already be reachable from `wrapper`, which
    // is what keeps them and their messages alive.
    ActorState* newActor(const proto::ProtoObject* wrapper,
                         const proto::ProtoObject* initialValue,
                         const proto::ProtoMPSCQueue* high,
                         const proto::ProtoMPSCQueue* medium,
                         const proto::ProtoMPSCQueue* low);

    // Enqueue a message (newMessage above); schedule the actor if not
    // already running or scheduled. Callable from any thread.
    void send(proto::ProtoContext* ctx, ActorState* actor,
              ActorPriority priority, const proto::ProtoObject* message);

    struct Stats {
        unsigned numWorkers;
        long long messagesProcessed;
    };
    Stats stats() const;

    static unsigned configuredWorkerCount();

    // Entry point for the protoCore worker thread. Public so it can
    // be passed to ProtoSpace::newThread; not part of the API.
    void workerLoop(proto::ProtoContext* ctx);

    // Read-only access for the worker (it captured the blueprint at
    // start time but the engine pointer needs to be visible too).
    const ActiveCallContext* blueprint() const { return ccBlueprint_; }

private:
    ActorScheduler() = default;
    ~ActorScheduler() = default;
    ActorScheduler(const ActorScheduler&) = delete;
    ActorScheduler& operator=(const ActorScheduler&) = delete;

    void enqueueReady_(ActorState* actor, ActorPriority p);
    ActorState* popReady_(proto::ProtoContext* ctx);

    std::once_flag startFlag_;
    std::atomic<bool> started_{false};
    std::atomic<bool> shuttingDown_{false};

    proto::ProtoSpace* space_ = nullptr;
    const ActiveCallContext* ccBlueprint_ = nullptr;

    std::vector<const proto::ProtoThread*> workers_;

    std::mutex                      queueMtx_;
    std::condition_variable         queueCv_;
    std::deque<ActorState*>         ready_[3];

    std::mutex                                ownersMtx_;
    std::vector<std::unique_ptr<ActorState>>  owners_;

    std::atomic<long long> messagesProcessed_{0};
};

} // namespace protoClojure
