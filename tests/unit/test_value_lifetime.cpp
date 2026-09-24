// Values that protoCore used to intern are ordinary garbage now: map keys
// (src/runtime/MapOps.h) and vectors (src/runtime/VectorOps.h).
//
// The layer this one replaced reduced every key to an INTERNED canonical key
// — a protoCore symbol for a string, an interned tuple for a collection — and
// protoCore frees neither. Every distinct key a program ever used stayed in
// memory until the process exited, keys of dead maps and keys that were only
// looked up included (the deviation recorded as D23, now withdrawn).
//
// This is a test with a premise, not an argument: it churns forty thousand
// distinct collection keys through short-lived maps, forces collections, and
// fails if the live set grows with the number of distinct keys. Restoring the
// interning — one newTupleFromList on the key before the store — makes it
// fail with the live set up by exactly one cell per key, which is how it was
// checked.
//
// String keys are not covered here, for a reason worth recording: protoCore's
// symbol table is invisible to both `heapSize` and `liveCellsLastCycle`, so
// re-interning string keys changes neither number (it only makes the run 90
// times slower). That half of D23 is covered instead by the white-box tests
// in test_mapops.cpp, which assert that a string key is stored as the string
// object the caller passed, under a hashed slot — i.e. that nothing on the
// key path calls createSymbol any more.
//
// The forced-collection pattern is protoCore's own
// (test/ProtoMapGCTests.cpp): shrink the hard heap limit to just above the
// current heap, allocate throwaway objects until enough cycles have run, then
// lift the limit again.

#include "runtime/MapOps.h"
#include "runtime/VectorOps.h"

#include "protoCore.h"
#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

namespace {

constexpr int kHeadroomCells   = 40000;
constexpr int kGarbagePerBatch = 5000;
constexpr int kWarmupKeys      = 2000;
constexpr int kChurnKeys       = 40000;

bool waitForIdleCollector(proto::ProtoSpace& space, proto::ProtoContext* ctx) {
    proto::ProtoContext::UnmanagedScope parked(ctx);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (space.gcStarted.load()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// The live set after at least `minCycles` collections, in cells.
unsigned long liveCellsAfterCollections(proto::ProtoSpace& space, proto::ProtoContext* live,
                                        std::uint64_t minCycles) {
    const std::uint64_t start = space.getGCCycleCount();
    space.setHeapLimits(/*soft=*/0, /*hard=*/space.heapSize + kHeadroomCells);
    for (int batch = 0; batch < 400 && space.getGCCycleCount() - start < minCycles; ++batch) {
        proto::ProtoContext garbage(&space, live, nullptr, nullptr, nullptr, nullptr);
        for (int i = 0; i < kGarbagePerBatch; ++i) (void) garbage.newObject(false);
    }
    space.setHeapLimits(0, 0);
    waitForIdleCollector(space, live);
    return space.liveCellsLastCycle.load();
}

// Churns `n` distinct one-entry maps, each under its own two-element list
// key — the shape whose canonical form used to be an interned ProtoTuple —
// in batch contexts, so the collector is shown them. Nothing survives.
void churnCollectionKeys(proto::ProtoSpace& space, proto::ProtoContext* parent, int from, int n) {
    for (int base = 0; base < n; base += 500) {
        proto::ProtoContext batch(&space, parent, nullptr, nullptr, nullptr, nullptr);
        const int end = base + 500 < n ? base + 500 : n;
        for (int i = base; i < end; ++i) {
            const proto::ProtoObject* key = batch.newList()
                                                ->appendLast(&batch, batch.fromLong(from + i))
                                                ->appendLast(&batch, batch.fromLong(from + i + 1))
                                                ->asObject(&batch);
            const proto::ProtoObject* kv[2] = {key, batch.fromLong(i)};
            ASSERT_EQ(protoClojure::mapCount(
                          &batch, protoClojure::mapAssocPairs(&batch, nullptr, kv, 2)),
                      1u);
        }
    }
}

// The live set may not grow by more than this per distinct key churned. A
// retained interned tuple costs one cell per key, so a tenth of a cell is far
// below "retained" and far above the noise of the warm-up.
double perKeyBudget() { return 0.1; }

}  // namespace

TEST(MapKeyLifetime, DistinctCollectionKeysDoNotAccumulate) {
    proto::ProtoSpace space;
    proto::ProtoContext live(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);

    churnCollectionKeys(space, &live, 0, kWarmupKeys);
    const unsigned long before = liveCellsAfterCollections(space, &live, 3);

    churnCollectionKeys(space, &live, 1000000, kChurnKeys);
    const unsigned long after = liveCellsAfterCollections(space, &live, 3);

    const double growth = static_cast<double>(after) - static_cast<double>(before);
    EXPECT_LT(growth, perKeyBudget() * kChurnKeys)
        << "the live set went from " << before << " to " << after << " cells over "
        << kChurnKeys << " distinct collection keys: keys are being retained";
}

// A vector used to be an interned ProtoTuple, which protoCore never frees, so
// every vector a program ever built stayed in memory until it exited
// (protoScala DESIGN risk R2). It is now a ProtoList in a one-entry
// sparse-list box, and both are collected. Building the box's payload with
// ctx->newTupleFromList instead makes this fail with the live set up by about
// one cell per element, which is how it was checked.
namespace {

void churnVectors(proto::ProtoSpace& space, proto::ProtoContext* parent, int from, int n) {
    for (int base = 0; base < n; base += 500) {
        proto::ProtoContext batch(&space, parent, nullptr, nullptr, nullptr, nullptr);
        const int end = base + 500 < n ? base + 500 : n;
        for (int i = base; i < end; ++i) {
            // Four distinct elements, so the vector is not one protoCore
            // would have shared with an earlier one anyway.
            const proto::ProtoList* items = batch.newList()
                                                ->appendLast(&batch, batch.fromLong(from + i))
                                                ->appendLast(&batch, batch.fromLong(from + i + 1))
                                                ->appendLast(&batch, batch.fromLong(from + i + 2))
                                                ->appendLast(&batch, batch.fromLong(from + i + 3));
            const proto::ProtoObject* v = protoClojure::newVector(&batch, items);
            ASSERT_TRUE(protoClojure::isVector(v));
            ASSERT_EQ(protoClojure::vectorItems(&batch, v)->getSize(&batch), 4u);
        }
    }
}

}  // namespace

TEST(VectorLifetime, DistinctVectorsDoNotAccumulate) {
    proto::ProtoSpace space;
    proto::ProtoContext live(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);

    churnVectors(space, &live, 0, kWarmupKeys);
    const unsigned long before = liveCellsAfterCollections(space, &live, 3);

    churnVectors(space, &live, 1000000, kChurnKeys);
    const unsigned long after = liveCellsAfterCollections(space, &live, 3);

    const double growth = static_cast<double>(after) - static_cast<double>(before);
    EXPECT_LT(growth, perKeyBudget() * kChurnKeys)
        << "the live set went from " << before << " to " << after << " cells over "
        << kChurnKeys << " distinct vectors: vectors are being retained";
}

TEST(VectorRepresentation, IsVectorIsATagTestAndItemsAreNotCopied) {
    proto::ProtoSpace space;
    proto::ProtoContext* ctx = space.rootContext;
    const proto::ProtoList* items =
        ctx->newList()->appendLast(ctx, ctx->fromLong(1))->appendLast(ctx, ctx->fromLong(2));
    const proto::ProtoObject* v = protoClojure::newVector(ctx, items);

    EXPECT_TRUE(protoClojure::isVector(v));
    // The box stores the list as it is: building a vector is O(1).
    EXPECT_EQ(protoClojure::vectorItems(ctx, v), items);
    EXPECT_EQ(protoClojure::vectorItems(ctx, v)->getSize(ctx), 2u);
    EXPECT_TRUE(protoClojure::isVector(protoClojure::newVector(ctx, ctx->newList())));

    // Nothing else the runtime hands out is a vector, and in particular not a
    // map (which is what used to occupy this pointer tag) or a list.
    EXPECT_FALSE(protoClojure::isVector(nullptr));
    EXPECT_FALSE(protoClojure::isVector(PROTO_NONE));
    EXPECT_FALSE(protoClojure::isVector(items->asObject(ctx)));
    EXPECT_FALSE(protoClojure::isVector(ctx->newMap()->asObject(ctx)));
    EXPECT_FALSE(protoClojure::isVector(ctx->fromLong(7)));
    EXPECT_FALSE(protoClojure::isVector(
        reinterpret_cast<const proto::ProtoObject*>(proto::ProtoString::fromUTF8(ctx, "ab"))));
    EXPECT_FALSE(protoClojure::isMap(v)) << "a vector must not read as a map";
}
