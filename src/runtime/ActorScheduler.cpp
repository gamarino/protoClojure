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

void ActorScheduler::send(ActorState* actor, ActorMessage&& msg) {
    ActorPriority schedPriority = msg.priority;
    {
        std::lock_guard<std::mutex> g(actor->mtx);
        actor->mailbox.push_back(std::move(msg));
        if (actor->running || actor->scheduled) return;
        actor->scheduled = true;
    }
    enqueueReady_(actor, schedPriority);
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

void ActorScheduler::workerLoop(proto::ProtoContext* ctx) {
    setActiveCallContext(*ccBlueprint_);

    for (;;) {
        ActorState* actor = popReady_();
        if (!actor) {
            clearActiveCallContext();
            return;
        }

        // SINGLE-METHOD INVARIANT: while `running` is true, no other
        // worker will pick this actor up — senders that arrive during
        // a drain see `running=true` and skip the schedule branch.
        // We clear `running` once at the end of the batch, in the
        // same lock-acquire that decides whether to re-enqueue. If we
        // cleared it after each message, a sender could observe
        // running=false / scheduled=false in the gap between two
        // messages and schedule the actor on a second worker — a
        // race we hit and fixed in v0.19.
        {
            std::lock_guard<std::mutex> g(actor->mtx);
            actor->running = true;
            actor->scheduled = false;
        }

        constexpr int kDrainBatch = 8;
        for (int k = 0; k < kDrainBatch; ++k) {
            ActorMessage msg;
            bool haveMsg = false;
            {
                std::lock_guard<std::mutex> g(actor->mtx);
                if (!actor->mailbox.empty()) {
                    msg = std::move(actor->mailbox.front());
                    actor->mailbox.pop_front();
                    haveMsg = true;
                }
            }
            if (!haveMsg) break;

            const proto::ProtoObject* result = PROTO_NONE;
            try {
                const proto::ProtoObject* buf[17];
                unsigned int total = 1;
                buf[0] = actor->value ? actor->value : PROTO_NONE;
                if (msg.args) {
                    const proto::ProtoList* alist = msg.args->asList(ctx);
                    unsigned long an = alist->getSize(ctx);
                    if (an > 15) an = 15;
                    for (unsigned long i = 0; i < an; ++i) {
                        buf[total++] = alist->getAt(ctx, (int)i);
                    }
                }
                result = ccBlueprint_->engine->invoke(ctx, msg.fn, buf, total);
            } catch (...) {
                result = PROTO_NONE;
            }

            actor->value = result;
            if (actor->wrapper) {
                const_cast<proto::ProtoObject*>(actor->wrapper)
                    ->setAttribute(ctx, ccBlueprint_->valueKey, result);
            }
            if (msg.promise) {
                proto::ProtoObject* p =
                    const_cast<proto::ProtoObject*>(msg.promise);
                bool first = p->setAttributeIfEqual(
                    ctx, ccBlueprint_->doneKey, PROTO_FALSE, PROTO_TRUE);
                if (first) {
                    p->setAttribute(ctx, ccBlueprint_->valueKey, result);
                }
            }

            messagesProcessed_.fetch_add(1, std::memory_order_relaxed);
        }

        // End of batch — atomically clear running and re-enqueue if
        // more messages arrived during the drain. Holding the mutex
        // here closes the window for a concurrent sender to schedule
        // a second worker on this actor.
        ActorPriority reschedAt = ActorPriority::Medium;
        bool reschedule = false;
        {
            std::lock_guard<std::mutex> g(actor->mtx);
            actor->running = false;
            if (!actor->mailbox.empty()) {
                reschedAt = actor->mailbox.front().priority;
                actor->scheduled = true;
                reschedule = true;
            }
        }
        if (reschedule) enqueueReady_(actor, reschedAt);
    }
}

ActorScheduler::Stats ActorScheduler::stats() const {
    return Stats{(unsigned)workers_.size(),
                 messagesProcessed_.load(std::memory_order_relaxed)};
}

} // namespace protoClojure
