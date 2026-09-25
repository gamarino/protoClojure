#include "GCCensus.h"

#include "protoCore.h"

#include <algorithm>
#include <chrono>
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

GCCensus::GCCensus(proto::ProtoSpace& space) : space_(space) {
    if (!gcCensusEnabled()) return;
    enabled_     = true;
    heapAtStart_ = space_.heapSize;
    sampler_     = std::thread([this] { sample(); });
}

void GCCensus::sample() {
    // protoCore increments gcCycleCount at the START of a cycle and publishes
    // reclaimedLastCycle at its END. So when the counter is observed to move
    // from N to N+1, reclaimedLastCycle still holds cycle N's result: reading
    // it at the transition attributes the number to the cycle that produced
    // it. The poll interval is short enough that a cycle is not usually
    // missed, and a missed one only under-reports the total — it can never
    // invent reclamation that did not happen.
    std::uint64_t last = space_.getGCCycleCount();
    while (!stop_.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        const std::uint64_t now = space_.getGCCycleCount();
        if (now == last) continue;
        const unsigned long r = space_.reclaimedLastCycle.load();
        reclaimedTotal_ += r;
        reclaimedMax_ = std::max(reclaimedMax_, r);
        ++observedCycles_;
        last = now;
    }
    // The cycle in flight when sampling stopped, if it has since published.
    const unsigned long tail = space_.reclaimedLastCycle.load();
    if (space_.getGCCycleCount() != last) {
        reclaimedTotal_ += tail;
        ++observedCycles_;
    }
    reclaimedMax_ = std::max(reclaimedMax_, tail);
}

GCCensus::~GCCensus() {
    if (!enabled_) return;
    stop_.store(true, std::memory_order_relaxed);
    if (sampler_.joinable()) sampler_.join();

    const unsigned long mean =
        observedCycles_ ? reclaimedTotal_ / observedCycles_ : 0UL;
    std::fprintf(stderr,
                 "protoclj gc: cycles=%llu reclaimed-total=%lu "
                 "reclaimed-max=%lu reclaimed-mean=%lu live-last=%lu "
                 "heap=%d heap-start=%d\n",
                 static_cast<unsigned long long>(space_.getGCCycleCount()),
                 reclaimedTotal_, reclaimedMax_, mean,
                 space_.liveCellsLastCycle.load(),
                 space_.heapSize, heapAtStart_);
}

}  // namespace protoClojure
