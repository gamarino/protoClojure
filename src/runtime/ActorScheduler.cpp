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
    {
        // A joining thread still counts in protoCore's runningThreads, so it never
        // parks: the stop-the-world quorum can never be met, no collection cycle can
        // start, and the thread being joined waits for memory that a cycle would have
        // freed. Leaving the running set for the duration of the block is what makes
        // this a join and not a deadlock. Pinned by tests/cli/blocking-joins-park-for-gc.sh.
        proto::ProtoContext::UnmanagedScope parked(ctx);
        for (auto* t : workers_) {
            if (t) const_cast<proto::ProtoThread*>(t)->join(ctx);
        }
    }
    workers_.clear();
}

// A message is a protoCore value, so nothing here owns heap nodes: an
// undrained mailbox is collected with the actor's wrapper.
const proto::ProtoObject* newMessage(proto::ProtoContext* ctx,
                                     const proto::ProtoObject* fn,
                                     const proto::ProtoObject* args,
                                     const proto::ProtoObject* promise) {
    const proto::ProtoObject* slots[message::kSize] = {
        fn ? fn : PROTO_NONE,
        args ? args : PROTO_NONE,
        promise ? promise : PROTO_NONE};
    return ctx->newList(message::kSize, slots)->asObject(ctx);
}

ActorState* ActorScheduler::newActor(const proto::ProtoObject* wrapper,
                                      const proto::ProtoObject* initialValue,
                                      const proto::ProtoMPSCQueue* high,
                                      const proto::ProtoMPSCQueue* medium,
                                      const proto::ProtoMPSCQueue* low) {
    auto state = std::make_unique<ActorState>();
    state->wrapper = wrapper;
    state->value = initialValue;
    state->mailbox[(int)ActorPriority::High]   = high;
    state->mailbox[(int)ActorPriority::Medium] = medium;
    state->mailbox[(int)ActorPriority::Low]    = low;
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

// Pick the highest non-empty priority among the actor's three mailboxes.
// Used by the running worker at end-of-batch, to decide where to
// re-enqueue an actor whose mailbox grew during the drain. Cheap: at
// most 3 atomic loads (ProtoMPSCQueue::isEmpty is one relaxed load).
static ActorPriority highestPendingBand(proto::ProtoContext* ctx,
                                        const ActorState* actor) {
    if (!actor->mailbox[0]->isEmpty(ctx)) return ActorPriority::High;
    if (!actor->mailbox[1]->isEmpty(ctx)) return ActorPriority::Medium;
    return ActorPriority::Low;
}

static bool anyMailboxPending(proto::ProtoContext* ctx, const ActorState* actor) {
    for (int band = 0; band < 3; ++band)
        if (!actor->mailbox[band]->isEmpty(ctx)) return true;
    return false;
}

void ActorScheduler::send(proto::ProtoContext* ctx, ActorState* actor,
                          ActorPriority band, const proto::ProtoObject* message) {
    // Lock-free, O(1), one cell. The message is a young cell of `ctx`
    // until this returns, and reachable through the queue afterwards, so
    // it is never invisible to the collector.
    actor->mailbox[(int)band]->push(ctx, message);

    // Claim the actor for scheduling. If we win the CAS, we own the
    // ready-queue insertion; otherwise the currently-running or
    // already-queued handler will see our message when it drains.
    bool expectedClaim = false;
    if (actor->claimed.compare_exchange_strong(
                expectedClaim, true,
                std::memory_order_acq_rel)) {
        enqueueReady_(actor, band);
    }
}

// An idle worker must leave the GC quorum while it waits.
//
// Workers are protoCore threads (ProtoSpace::newThread), so a
// stop-the-world phase waits for every one of them to park at a
// safepoint. A std::condition_variable wait is not a safepoint: a worker
// blocked in one would never park, and the collector would wait for it
// for ever. That is a deadlock of the whole process, and it needs only
// an idle worker and a collection — which is why it took a heap ceiling
// (tests/cli/actor-payloads-survive-gc.sh) to show up.
//
// UnmanagedScope marks the thread "permanently parked" for quorum
// purposes, exactly as the promise poll in `deref` does. Two orderings
// matter and are both load bearing:
//   * the wait happens with NO ProtoObject touched, as the unmanaged
//     contract requires;
//   * `lk` is declared after `parked`, so the mutex is released BEFORE
//     returnFromUnmanaged, which blocks until a stop-the-world in
//     progress clears. Holding queueMtx_ across that block would stall
//     every sender in enqueueReady_ — and a sender is a managed thread,
//     so the collection could never finish either.
ActorState* ActorScheduler::popReady_(proto::ProtoContext* ctx) {
    for (;;) {
        {
            std::lock_guard<std::mutex> g(queueMtx_);
            for (int p = 0; p < 3; ++p) {
                if (!ready_[p].empty()) {
                    ActorState* a = ready_[p].front();
                    ready_[p].pop_front();
                    return a;
                }
            }
            if (shuttingDown_) return nullptr;  // shutting down with empty queues
        }
        proto::ProtoContext::UnmanagedScope parked(ctx);
        std::unique_lock<std::mutex> lk(queueMtx_);
        queueCv_.wait(lk, [this]() {
            if (shuttingDown_) return true;
            for (int p = 0; p < 3; ++p) if (!ready_[p].empty()) return true;
            return false;
        });
    }
}

void ActorScheduler::workerLoop(proto::ProtoContext* ctx) {
    setActiveCallContext(*ccBlueprint_);

    for (;;) {
        ActorState* actor = popReady_(ctx);
        if (!actor) {
            clearActiveCallContext();
            return;
        }

        // SINGLE-METHOD INVARIANT: `claimed` was set to true by the
        // sender (or by the previous turn's end-of-batch re-enqueue)
        // and stays true for as long as this worker holds the actor.
        // Concurrent senders observe `claimed == true`, push onto the
        // band's queue, and skip the ready-queue insertion — we will
        // see their message when we drain again below, or at
        // end-of-batch.

        // Drain in strict priority order: High first, then Medium, then
        // Low. Within a band, FIFO. A new High-priority message that
        // arrives mid-drain will be picked up either at end-of-batch
        // re-enqueue, or — if it arrives BEFORE we drain its band on
        // this turn — folded into the same batch.
        //
        // The whole turn runs in its own context: takeAll's batch list,
        // the values the handlers build and the cells the mailbox nodes
        // leave behind are that context's young generation, and
        // protoCore shows a context's allocations to the collector when
        // it is destroyed. A worker thread lives for the process, so
        // draining through the thread's own context would pin every
        // message it ever handled.
        {
            proto::ProtoContext turn(ctx->space, ctx);
            // The three batches, rooted in slots while they are walked:
            // a handler may run arbitrary Clojure code, including one
            // that drops the only other reference to a message.
            turn.resizeAutomaticLocals(3);
            for (int band = 0; band < 3; ++band) {
                turn.setAutomaticLocal(
                    band, actor->mailbox[band]->takeAll(&turn)->asObject(&turn));
            }

            for (int band = 0; band < 3; ++band) {
                const proto::ProtoList* batch =
                    turn.getAutomaticLocal(band)->asList(&turn);
                const unsigned long count = batch->getSize(&turn);
                for (unsigned long at = 0; at < count; ++at) {
                    const proto::ProtoList* msg =
                        batch->getAt(&turn, (int)at)->asList(&turn);
                    const proto::ProtoObject* fn = msg->getAt(&turn, message::kFn);
                    const proto::ProtoObject* margs = msg->getAt(&turn, message::kArgs);
                    const proto::ProtoObject* promise =
                        msg->getAt(&turn, message::kPromise);

                    const proto::ProtoObject* result = PROTO_NONE;
                    try {
                        const proto::ProtoObject* buf[17];
                        unsigned int total = 1;
                        buf[0] = actor->value ? actor->value : PROTO_NONE;
                        if (margs && margs != PROTO_NONE) {
                            const proto::ProtoList* alist = margs->asList(&turn);
                            unsigned long an = alist->getSize(&turn);
                            if (an > 15) an = 15;
                            for (unsigned long i = 0; i < an; ++i) {
                                buf[total++] = alist->getAt(&turn, (int)i);
                            }
                        }
                        result = ccBlueprint_->engine->invoke(&turn, fn, buf, total);
                    } catch (...) {
                        result = PROTO_NONE;
                    }

                    // The wrapper is mutable, so this store is what keeps
                    // the new value alive past the end of this turn; the
                    // raw `value` pointer is only a cache of it.
                    actor->value = result;
                    if (actor->wrapper) {
                        const_cast<proto::ProtoObject*>(actor->wrapper)
                            ->setAttribute(&turn, ccBlueprint_->valueKey, result);
                    }
                    if (promise && promise != PROTO_NONE) {
                        // One compare-and-set, as `deliver` does: a concurrent
                        // deref never sees the promise realized without its
                        // value.
                        deliverPromise(&turn, ccBlueprint_->valueKey, promise, result);
                    }

                    messagesProcessed_.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        // End of batch — drop the claim. A sender that pushes BEFORE
        // this store will see claimed==true and skip the schedule;
        // their message will be observed by the isEmpty checks below. A
        // sender that pushes AFTER this store will race for the claim;
        // if they win, they enqueue; if we win, we re-enqueue here.
        actor->claimed.store(false, std::memory_order_release);

        // Did anything arrive while we were draining? Check all three
        // bands so a late high-priority send is re-enqueued at the
        // right priority. The CAS races the sender's CAS-claim — at
        // most one of us wins.
        if (anyMailboxPending(ctx, actor)) {
            bool expected = false;
            if (actor->claimed.compare_exchange_strong(
                        expected, true, std::memory_order_acq_rel)) {
                enqueueReady_(actor, highestPendingBand(ctx, actor));
            }
        }
    }
}

ActorScheduler::Stats ActorScheduler::stats() const {
    return Stats{(unsigned)workers_.size(),
                 messagesProcessed_.load(std::memory_order_relaxed)};
}

} // namespace protoClojure
