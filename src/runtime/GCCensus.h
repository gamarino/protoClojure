/*
 * GCCensus — a diagnostic census of protoCore's collector, for one run.
 *
 * protoCore publishes the collector's work as three atomics on the
 * ProtoSpace: a monotonic cycle counter, the cells the last completed cycle
 * reclaimed, and the cells it found live. Only the counter is cumulative:
 * `reclaimedLastCycle` is overwritten by every cycle, so a run that ends
 * having reclaimed millions of cells and a run that reclaimed nothing at all
 * can print the same final value. Deciding whether the collector is working
 * therefore needs the per-cycle values sampled as they are published.
 *
 * That is all this does. While it is alive it polls those atomics on a
 * thread of its own, attributes each `reclaimedLastCycle` to the cycle that
 * produced it, and on destruction prints one line to standard error:
 *
 *   protoclj gc: cycles=N reclaimed-total=T reclaimed-max=M reclaimed-mean=A
 *                live-last=L heap=H heap-start=S
 *
 * `heap` is the high-water mark by construction — protoCore's `heapSize`
 * only ever grows — so `heap` against `heap-start` is the heap growth of the
 * run, and a collector that reclaims nothing shows up as `reclaimed-max=0`
 * with `heap` far above `heap-start`.
 *
 * Enabled only by PROTOCLJ_GC_STATS=1; constructing it without that costs a
 * getenv and starts no thread. The sampler is deliberately NOT a registered
 * protoCore thread: it touches no `ProtoObject*`, allocates nothing through
 * a context and reads only `std::atomic` members of the space, so it neither
 * needs to park for a stop-the-world nor can be seen by one. It is joined
 * before the ProtoSpace it samples is destroyed.
 *
 * Diagnostic only: nothing in the runtime's behaviour depends on it.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

namespace proto {
class ProtoSpace;
}

namespace protoClojure {

// True when PROTOCLJ_GC_STATS is set to anything but "0" or the empty
// string. Read once, on the first call.
bool gcCensusEnabled();

class GCCensus {
public:
    // Starts sampling `space` when gcCensusEnabled(); otherwise inert.
    explicit GCCensus(proto::ProtoSpace& space);

    // Stops the sampler, joins it, and prints the census line.
    ~GCCensus();

    GCCensus(const GCCensus&)            = delete;
    GCCensus& operator=(const GCCensus&) = delete;

private:
    void sample();

    proto::ProtoSpace& space_;
    bool               enabled_ = false;
    int                heapAtStart_ = 0;
    std::atomic<bool>  stop_{false};
    std::thread        sampler_;

    // Written by the sampler thread, read after the join.
    std::uint64_t  observedCycles_  = 0;
    unsigned long  reclaimedTotal_  = 0;
    unsigned long  reclaimedMax_    = 0;
};

}  // namespace protoClojure
