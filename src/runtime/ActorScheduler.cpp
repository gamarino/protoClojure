#include "ActorScheduler.h"
#include "Primitives.h"
#include "ExecutionEngine.h"

#include "protoCore.h"

#include <chrono>
#include <cstdlib>
#include <thread>

namespace protoClojure {

ActorScheduler& ActorScheduler::instance() {
    static ActorScheduler s;
    return s;
}

unsigned ActorScheduler::configuredWorkerCount() {
    const char* env = std::getenv("PROTOCLJ_ACTOR_WORKERS");
    if (env && *env) {
        int n = std::atoi(env);
        if (n >= 1 && n <= 64) return (unsigned)n;
    }
    unsigned hc = std::thread::hardware_concurrency();
    if (hc == 0) hc = 4;
    unsigned n = hc > 2 ? hc - 2 : 2;
    if (n > 16) n = 16;
    return n;
}

// Static thunk used as the ProtoMethod entry point for protoCore's
// newThread. Reads the scheduler pointer from args[0] (as a long)
// and dispatches to workerLoop on its own ProtoContext.
static const proto::ProtoObject* actorWorkerEntry(
        proto::ProtoContext* ctx,
        const proto::ProtoObject* /*self*/,
        const proto::ParentLink*,
        const proto::ProtoList* args,
        const proto::ProtoSparseList*) {
    if (!args || args->getSize(ctx) == 0) return PROTO_NONE;
    const proto::ProtoObject* sched = args->getAt(ctx, 0);
    if (!sched || !sched->isInteger(ctx)) return PROTO_NONE;
    ActorScheduler* s =
        reinterpret_cast<ActorScheduler*>(sched->asLong(ctx));
    s->workerLoop(ctx);
    return PROTO_NONE;
}

void ActorScheduler::ensureStarted(proto::ProtoSpace* space,
                                    proto::ProtoContext* mainCtx,
                                    const ActiveCallContext& cc) {
    std::call_once(startFlag_, [&]() {
        space_ = space;
        static ActiveCallContext s_blueprint = cc;
        ccBlueprint_ = &s_blueprint;
        started_ = true;
        unsigned n = configuredWorkerCount();
        const proto::ProtoString* name =
            proto::ProtoString::createSymbol(mainCtx, "protoclj-actor-worker");
        const proto::ProtoObject* schedHandle =
            mainCtx->fromLong(reinterpret_cast<long long>(this));
        for (unsigned i = 0; i < n; ++i) {
            const proto::ProtoList* targs =
                mainCtx->newList()->appendLast(mainCtx, schedHandle);
            const proto::ProtoThread* t =
                space_->newThread(mainCtx, name, &actorWorkerEntry,
                                   targs, nullptr);
            workers_.push_back(t);
        }
    });
}

void ActorScheduler::shutdown(proto::ProtoContext* ctx) {
    if (!started_) return;
    shuttingDown_ = true;
    queueCv_.notify_all();
    for (auto* t : workers_) {
        if (t) const_cast<proto::ProtoThread*>(t)->join(ctx);
    }
    workers_.clear();
}

// Free any messages still in flight when an actor is dropped at
// shutdown (typically none — the scheduler drains workers first —
// but during a forced shutdown a sender might race with the join).
ActorState::~ActorState() {
    for (int band = 0; band < 3; ++band) {
        ActorMessage* m = head[band].load(std::memory_order_acquire);
        while (m) {
            ActorMessage* next = m->next;
            delete m;
            m = next;
        }
    }
}

ActorState* ActorScheduler::newActor(const proto::ProtoObject* wrapper,
                                      const proto::ProtoObject* initialValue) {
    auto state = std::make_unique<ActorState>();
    state->wrapper = wrapper;
    state->value = initialValue;
    ActorState* ptr = state.get();
    std::lock_guard<std::mutex> g(ownersMtx_);
    owners_.push_back(std::move(state));
    return ptr;
}

void ActorScheduler::enqueueReady_(ActorState* actor, ActorPriority p) {
    std::lock_guard<std::mutex> g(queueMtx_);
    ready_[(int)p].push_back(actor);
    queueCv_.notify_one();
}

// Pick the highest non-empty priority among the actor's three heads.
// Used both by the running worker at end-of-batch (to decide where to
// re-enqueue an actor whose mailbox grew during the drain) and by
// senders that lost the claim race but want their priority known.
// Cheap: at most 3 atomic loads.
static ActorPriority highestPendingBand(const ActorState* actor) {
    if (actor->head[0].load(std::memory_order_acquire)) return ActorPriority::High;
    if (actor->head[1].load(std::memory_order_acquire)) return ActorPriority::Medium;
    return ActorPriority::Low;
}

void ActorScheduler::send(ActorState* actor, ActorMessage&& msg) {
    ActorPriority band = msg.priority;
    ActorMessage* node = new ActorMessage(std::move(msg));
    // CAS-push onto the per-band stack. node->next captures the current
    // head; the CAS retries on contention. Release on success so the
    // worker sees the fully-initialised node via its acquire-exchange.
    ActorMessage* expected = actor->head[(int)band]
                                   .load(std::memory_order_relaxed);
    do {
        node->next = expected;
    } while (!actor->head[(int)band].compare_exchange_weak(
                expected, node,
                std::memory_order_release,
                std::memory_order_relaxed));

    // Claim the actor for scheduling. If we win the CAS, we own the
    // ready-queue insertion; otherwise the currently-running or
    // already-queued handler will see our node when it drains.
    bool expectedClaim = false;
    if (actor->claimed.compare_exchange_strong(
                expectedClaim, true,
                std::memory_order_acq_rel)) {
        enqueueReady_(actor, band);
    }
}

ActorState* ActorScheduler::popReady_() {
    std::unique_lock<std::mutex> lk(queueMtx_);
    queueCv_.wait(lk, [this]() {
        if (shuttingDown_) return true;
        for (int p = 0; p < 3; ++p) if (!ready_[p].empty()) return true;
        return false;
    });
    for (int p = 0; p < 3; ++p) {
        if (!ready_[p].empty()) {
            ActorState* a = ready_[p].front();
            ready_[p].pop_front();
            return a;
        }
    }
    return nullptr;  // shutting down with empty queues
}

// Drain one priority band's mailbox: atomically pluck the whole stack,
// reverse it (LIFO → FIFO), and return the head of the reversed list.
// Caller owns the freed nodes.
static ActorMessage* drainBand(ActorState* actor, int band) {
    ActorMessage* lifo =
        actor->head[band].exchange(nullptr, std::memory_order_acquire);
    ActorMessage* fifo = nullptr;
    while (lifo) {
        ActorMessage* next = lifo->next;
        lifo->next = fifo;
        fifo = lifo;
        lifo = next;
    }
    return fifo;
}

void ActorScheduler::workerLoop(proto::ProtoContext* ctx) {
    setActiveCallContext(*ccBlueprint_);

    for (;;) {
        ActorState* actor = popReady_();
        if (!actor) {
            clearActiveCallContext();
            return;
        }

        // SINGLE-METHOD INVARIANT: `claimed` was set to true by the
        // sender (or by the previous turn's end-of-batch re-enqueue)
        // and stays true for as long as this worker holds the actor.
        // Concurrent senders observe `claimed == true`, push their
        // node onto the per-band stack, and skip the ready-queue
        // insertion — we will see their message when we drain again
        // below, or at end-of-batch.

        // Drain in strict priority order: High first, then Medium, then
        // Low. Within a band, FIFO. A new High-priority message that
        // arrives mid-drain will be picked up either at end-of-batch
        // re-enqueue, or — if it arrives BEFORE we drain its band on
        // this turn — folded into the same batch.
        ActorMessage* head[3] = {
            drainBand(actor, 0),
            drainBand(actor, 1),
            drainBand(actor, 2),
        };

        for (int band = 0; band < 3; ++band) {
            while (head[band]) {
                ActorMessage* msg = head[band];
                head[band] = msg->next;

                const proto::ProtoObject* result = PROTO_NONE;
                try {
                    const proto::ProtoObject* buf[17];
                    unsigned int total = 1;
                    buf[0] = actor->value ? actor->value : PROTO_NONE;
                    if (msg->args) {
                        const proto::ProtoList* alist = msg->args->asList(ctx);
                        unsigned long an = alist->getSize(ctx);
                        if (an > 15) an = 15;
                        for (unsigned long i = 0; i < an; ++i) {
                            buf[total++] = alist->getAt(ctx, (int)i);
                        }
                    }
                    result = ccBlueprint_->engine->invoke(
                        ctx, msg->fn, buf, total);
                } catch (...) {
                    result = PROTO_NONE;
                }

                actor->value = result;
                if (actor->wrapper) {
                    const_cast<proto::ProtoObject*>(actor->wrapper)
                        ->setAttribute(ctx, ccBlueprint_->valueKey, result);
                }
                if (msg->promise) {
                    proto::ProtoObject* p =
                        const_cast<proto::ProtoObject*>(msg->promise);
                    bool first = p->setAttributeIfEqual(
                        ctx, ccBlueprint_->doneKey,
                        PROTO_FALSE, PROTO_TRUE);
                    if (first) {
                        p->setAttribute(ctx, ccBlueprint_->valueKey, result);
                    }
                }

                messagesProcessed_.fetch_add(1, std::memory_order_relaxed);
                delete msg;
            }
        }

        // End of batch — drop the claim. A sender that pushes BEFORE
        // this store will see claimed==true and skip the schedule;
        // their message will be observed by the head.load below. A
        // sender that pushes AFTER this store will race for the claim;
        // if they win, they enqueue; if we win, we re-enqueue here.
        actor->claimed.store(false, std::memory_order_release);

        // Did anything arrive while we were draining? Check all three
        // bands so a late high-priority send is re-enqueued at the
        // right priority. The CAS races the sender's CAS-claim — at
        // most one of us wins.
        if (actor->head[0].load(std::memory_order_acquire) != nullptr
                || actor->head[1].load(std::memory_order_acquire) != nullptr
                || actor->head[2].load(std::memory_order_acquire) != nullptr) {
            bool expected = false;
            if (actor->claimed.compare_exchange_strong(
                        expected, true, std::memory_order_acq_rel)) {
                enqueueReady_(actor, highestPendingBand(actor));
            }
        }
    }
}

ActorScheduler::Stats ActorScheduler::stats() const {
    return Stats{(unsigned)workers_.size(),
                 messagesProcessed_.load(std::memory_order_relaxed)};
}

} // namespace protoClojure
