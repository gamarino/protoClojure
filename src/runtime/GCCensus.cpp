#include "GCCensus.h"

#include "protoCore.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace protoClojure {

bool gcCensusEnabled() {
    static const bool on = [] {
        const char* e = std::getenv("PROTOCLJ_GC_STATS");
        return e != nullptr && e[0] != '\0' && std::strcmp(e, "0") != 0;
    }();
    return on;
}

GCCensus::GCCensus(proto::ProtoContext* ctx) : ctx_(ctx), space_(*ctx->space) {
    if (!gcCensusEnabled()) return;
    enabled_     = true;
    heapAtStart_ = space_.heapSize;
    // `this` travels as an integer, as the future workers' arguments do.
    const proto::ProtoList* args =
        ctx->newList()->appendLast(ctx, ctx->fromLong(reinterpret_cast<long long>(this)));
    sampler_ = space_.newThread(ctx, proto::ProtoString::createSymbol(ctx, "protoclj-gc-census"),
                                &GCCensus::samplerMain, args, nullptr);
}

const proto::ProtoObject* GCCensus::samplerMain(proto::ProtoContext* ctx, const proto::ProtoObject*,
                                                const proto::ParentLink*,
                                                const proto::ProtoArguments* args,
                                                const proto::ProtoKeywordArguments*) {
    if (!args || args->getSize(ctx) == 0) return PROTO_NONE;
    auto* self = reinterpret_cast<GCCensus*>(args->getAt(ctx, 0)->asLong(ctx));
    self->sample(ctx);
    return PROTO_NONE;
}

void GCCensus::sample(proto::ProtoContext* ctx) {
    // protoCore increments gcCycleCount at the START of a cycle and publishes
    // reclaimedLastCycle at its END. So when the counter is observed to move
    // from N to N+1, reclaimedLastCycle still holds cycle N's result: reading
    // it at the transition attributes the number to the cycle that produced
    // it. The poll interval is short enough that a cycle is not usually
    // missed, and a missed one only under-reports the total — it can never
    // invent reclamation that did not happen.
    std::uint64_t last = space_.getGCCycleCount();
    while (!stop_.load(std::memory_order_relaxed)) {
        proto::ProtoThread::sleepFor(ctx, 200);
        const std::uint64_t now = space_.getGCCycleCount();
        if (now == last) continue;
        const proto::proto_ulong r = space_.reclaimedLastCycle.load();
        reclaimedTotal_ += r;
        reclaimedMax_ = std::max(reclaimedMax_, r);
        ++observedCycles_;
        last = now;
    }
    // The cycle in flight when sampling stopped, if it has since published.
    const proto::proto_ulong tail = space_.reclaimedLastCycle.load();
    if (space_.getGCCycleCount() != last) {
        reclaimedTotal_ += tail;
        ++observedCycles_;
    }
    reclaimedMax_ = std::max(reclaimedMax_, tail);
}

GCCensus::~GCCensus() {
    if (!enabled_) return;
    stop_.store(true, std::memory_order_relaxed);
    // join leaves the stop-the-world quorum for the wait itself.
    if (sampler_) const_cast<proto::ProtoThread*>(sampler_)->join(ctx_);

    const proto::proto_ulong mean =
        observedCycles_ ? reclaimedTotal_ / observedCycles_ : PROTO_UL(0);
    std::fprintf(stderr,
                 "protoclj gc: cycles=%llu reclaimed-total=%" PROTO_FMT_U " "
                 "reclaimed-max=%" PROTO_FMT_U " reclaimed-mean=%" PROTO_FMT_U
                 " live-last=%" PROTO_FMT_U " "
                 "heap=%d heap-start=%d\n",
                 static_cast<unsigned long long>(space_.getGCCycleCount()),
                 reclaimedTotal_, reclaimedMax_, mean,
                 space_.liveCellsLastCycle.load(),
                 space_.heapSize, heapAtStart_);
}

}  // namespace protoClojure
