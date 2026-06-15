/*
 * ActorScheduler — global worker pool for protoClojure actors.
 *
 * Three priority queues (High / Medium / Low) of READY actors;
 * `N` worker threads drain the highest-non-empty queue first. Each
 * actor has its own mailbox, drained one message at a time (the
 * single-method invariant — same actor never runs concurrently with
 * itself). Workers are protoCore threads (`ProtoSpace::newThread`)
 * so they participate in the GC quorum; mailboxes + ready queues
 * are C++ structures protected by std::mutex.
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
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace proto {
class ProtoContext;
class ProtoObject;
class ProtoSpace;
class ProtoList;
class ProtoThread;
}

namespace protoClojure {

struct ActiveCallContext;

enum class ActorPriority {
    High   = 0,
    Medium = 1,
    Low    = 2,
};

// One enqueued send. The promise will be delivered the result of
// `(f current-state args...)` once the message is processed.
struct ActorMessage {
    const proto::ProtoObject* fn;
    const proto::ProtoObject* args;     // ProtoList (may be nil)
    const proto::ProtoObject* promise;  // Promise to deliver into
    ActorPriority             priority;
};

// One actor's runtime state. Pinned by the wrapper ProtoObject's
// `__actor_state__` attribute (long pointer). The scheduler owns
// these via unique_ptr; they live for the process lifetime.
struct ActorState {
    std::mutex                  mtx;
    std::deque<ActorMessage>    mailbox;
    bool                        running    = false;
    bool                        scheduled  = false;
    const proto::ProtoObject*   value      = nullptr;  // mirrored under wrapper __value__
    const proto::ProtoObject*   wrapper    = nullptr;
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

    ActorState* newActor(const proto::ProtoObject* wrapper,
                         const proto::ProtoObject* initialValue);

    // Enqueue a message; schedule the actor if not already running
    // or scheduled.
    void send(ActorState* actor, ActorMessage&& msg);

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
    ActorState* popReady_();

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
