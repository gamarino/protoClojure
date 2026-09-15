// Unit tests for the runtime map (src/runtime/MapOps.h). The conformance
// fixtures under tests/conformance/16-maps cover the Clojure-visible
// behaviour; these tests cover what Clojure code cannot reach: hash-bucket
// collisions (forced through MapLayout::hash) and maps large enough to leave
// protoCore's small sparse-list form.

#include "runtime/MapOps.h"

#include "protoCore.h"
#include <gtest/gtest.h>

#include <vector>

using protoClojure::MapLayout;

namespace {

struct MapOpsFixture : ::testing::Test {
    proto::ProtoSpace space;
    proto::ProtoContext* ctx = space.rootContext;
    const proto::ProtoObject* marker =
        space.objectPrototype->newChild(ctx, /*isMutable=*/true);
    const proto::ProtoString* stateKey =
        proto::ProtoString::createSymbol(ctx, "__map__");
    MapLayout layout{marker, stateKey};

    const proto::ProtoObject* kw(const char* name) const {
        return reinterpret_cast<const proto::ProtoObject*>(
            proto::ProtoString::createSymbol(ctx, name));
    }
    const proto::ProtoObject* num(long long v) const { return ctx->fromLong(v); }

    // Keys and values in iteration order, flattened (k0 v0 k1 v1 ...).
    std::vector<const proto::ProtoObject*> walk(const proto::ProtoObject* m) const {
        std::vector<const proto::ProtoObject*> out;
        protoClojure::mapForEach(ctx, layout, m, &out,
            [](proto::ProtoContext*, void* self, const proto::ProtoObject* k,
               const proto::ProtoObject* v) {
                auto* o = static_cast<std::vector<const proto::ProtoObject*>*>(self);
                o->push_back(k);
                o->push_back(v);
            });
        return out;
    }

    const proto::ProtoObject* assoc(const proto::ProtoObject* m,
                                    const proto::ProtoObject* k,
                                    const proto::ProtoObject* v) const {
        const proto::ProtoObject* kv[2] = {k, v};
        return protoClojure::mapAssocPairs(ctx, layout, m, kv, 2);
    }
};

TEST_F(MapOpsFixture, IterationFollowsInsertionOrderBeyondTheSmallForm) {
    // 200 integer keys in a scrambled order: far past the inline small form
    // of a ProtoSparseList, so the ordered walk runs over the AVL form.
    const proto::ProtoObject* m = nullptr;
    std::vector<long long> order;
    for (long long i = 0; i < 200; ++i) {
        long long key = (i * 73) % 211;
        order.push_back(key);
        m = assoc(m, num(key), num(i));
    }
    auto entries = walk(m);
    ASSERT_EQ(entries.size(), 400u);
    for (std::size_t i = 0; i < order.size(); ++i) {
        EXPECT_EQ(entries[2 * i]->asLong(ctx), order[i]);
        EXPECT_EQ(entries[2 * i + 1]->asLong(ctx), static_cast<long long>(i));
    }
}

TEST_F(MapOpsFixture, AssocOfExistingKeyKeepsPositionAndKeyObject) {
    const proto::ProtoObject* m = nullptr;
    m = assoc(m, kw(":a"), num(1));
    m = assoc(m, kw(":b"), num(2));
    m = assoc(m, kw(":c"), num(3));
    const proto::ProtoObject* m2 = assoc(m, kw(":a"), num(9));
    auto e = walk(m2);
    ASSERT_EQ(e.size(), 6u);
    EXPECT_EQ(e[0], kw(":a"));
    EXPECT_EQ(e[1]->asLong(ctx), 9);
    EXPECT_EQ(e[2], kw(":b"));
    EXPECT_EQ(e[4], kw(":c"));
    // Persistence: the original map is unchanged.
    bool found = false;
    EXPECT_EQ(protoClojure::mapGet(ctx, layout, m, kw(":a"), &found)->asLong(ctx), 1);
    EXPECT_TRUE(found);
}

TEST_F(MapOpsFixture, DissocKeepsTheOrderOfTheRemainingKeys) {
    const proto::ProtoObject* m = nullptr;
    for (const char* k : {":d", ":a", ":c", ":b"}) m = assoc(m, kw(k), num(0));
    m = protoClojure::mapDissoc(ctx, layout, m, kw(":a"));
    auto e = walk(m);
    ASSERT_EQ(e.size(), 6u);
    EXPECT_EQ(e[0], kw(":d"));
    EXPECT_EQ(e[2], kw(":c"));
    EXPECT_EQ(e[4], kw(":b"));
    // A re-added key goes to the end.
    m = assoc(m, kw(":a"), num(1));
    e = walk(m);
    ASSERT_EQ(e.size(), 8u);
    EXPECT_EQ(e[6], kw(":a"));
}

TEST_F(MapOpsFixture, DissocOfAnAbsentKeyReturnsTheSameMap) {
    const proto::ProtoObject* m = assoc(nullptr, kw(":a"), num(1));
    EXPECT_EQ(protoClojure::mapDissoc(ctx, layout, m, kw(":zz")), m);
}

TEST_F(MapOpsFixture, HashCollisionsShareABucket) {
    // Every key hashes to the same value, so all entries share one bucket.
    MapLayout colliding{marker, stateKey,
        [](proto::ProtoContext*, const proto::ProtoObject*) -> unsigned long {
            return 42;
        }};
    auto assocC = [&](const proto::ProtoObject* m, const proto::ProtoObject* k,
                      long long v) {
        const proto::ProtoObject* kv[2] = {k, num(v)};
        return protoClojure::mapAssocPairs(ctx, colliding, m, kv, 2);
    };
    auto getC = [&](const proto::ProtoObject* m, const proto::ProtoObject* k,
                    bool* found) {
        return protoClojure::mapGet(ctx, colliding, m, k, found);
    };
    auto walkC = [&](const proto::ProtoObject* m) {
        std::vector<const proto::ProtoObject*> out;
        protoClojure::mapForEach(ctx, colliding, m, &out,
            [](proto::ProtoContext*, void* self, const proto::ProtoObject* k,
               const proto::ProtoObject* v) {
                auto* o = static_cast<std::vector<const proto::ProtoObject*>*>(self);
                o->push_back(k);
                o->push_back(v);
            });
        return out;
    };

    const proto::ProtoObject* a = kw(":x1");
    const proto::ProtoObject* b = kw(":x2");
    const proto::ProtoObject* c = kw(":x3");
    const proto::ProtoObject* m = nullptr;
    m = assocC(m, a, 1);
    m = assocC(m, b, 2);
    m = assocC(m, c, 3);
    m = assocC(m, b, 20);  // replace inside the bucket

    bool found = false;
    EXPECT_EQ(getC(m, a, &found)->asLong(ctx), 1);
    EXPECT_TRUE(found);
    EXPECT_EQ(getC(m, b, &found)->asLong(ctx), 20);
    EXPECT_EQ(getC(m, c, &found)->asLong(ctx), 3);
    getC(m, kw(":x4"), &found);
    EXPECT_FALSE(found);
    auto e = walkC(m);
    ASSERT_EQ(e.size(), 6u);
    EXPECT_EQ(e[0], a);
    EXPECT_EQ(e[2], b);
    EXPECT_EQ(e[3]->asLong(ctx), 20);
    EXPECT_EQ(e[4], c);

    // Remove the middle key of the bucket; the others stay reachable and
    // keep their order.
    m = protoClojure::mapDissoc(ctx, colliding, m, b);
    getC(m, b, &found);
    EXPECT_FALSE(found);
    EXPECT_EQ(getC(m, c, &found)->asLong(ctx), 3);
    e = walkC(m);
    ASSERT_EQ(e.size(), 4u);
    EXPECT_EQ(e[0], a);
    EXPECT_EQ(e[2], c);

    // Emptying the bucket removes it.
    m = protoClojure::mapDissoc(ctx, colliding, m, a);
    m = protoClojure::mapDissoc(ctx, colliding, m, c);
    EXPECT_TRUE(walkC(m).empty());
    getC(m, a, &found);
    EXPECT_FALSE(found);
}

TEST_F(MapOpsFixture, RepeatedKeyInOnePairListKeepsLastValueAndFirstPosition) {
    const proto::ProtoObject* kv[6] = {kw(":a"), num(1), kw(":b"), num(2),
                                       kw(":a"), num(3)};
    const proto::ProtoObject* m =
        protoClojure::mapAssocPairs(ctx, layout, nullptr, kv, 6);
    auto e = walk(m);
    ASSERT_EQ(e.size(), 4u);
    EXPECT_EQ(e[0], kw(":a"));
    EXPECT_EQ(e[1]->asLong(ctx), 3);
    EXPECT_EQ(e[2], kw(":b"));
}

} // namespace
