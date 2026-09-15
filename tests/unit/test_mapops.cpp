// Unit tests for the runtime map (src/runtime/MapOps.h). The conformance
// fixtures under tests/conformance/16-maps cover the Clojure-visible
// behaviour; these tests cover what Clojure code cannot reach: hash-bucket
// collisions (forced through MapLayout::hash) and maps large enough to leave
// protoCore's small sparse-list form.

#include "runtime/MapOps.h"

#include "protoCore.h"
#include <gtest/gtest.h>

#include <initializer_list>
#include <utility>
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

// Value comparator for mapEquals: recurses into maps built with the layout
// passed as `self`, otherwise protoCore compare.
bool eqValues(proto::ProtoContext* ctx, void* self,
              const proto::ProtoObject* a, const proto::ProtoObject* b) {
    const MapLayout& l = *static_cast<const MapLayout*>(self);
    if (protoClojure::isMap(ctx, l, a) && protoClojure::isMap(ctx, l, b))
        return protoClojure::mapEquals(ctx, l, a, b, self, &eqValues);
    return a->compare(ctx, b) == 0;
}

bool mapsEqual(proto::ProtoContext* ctx, const MapLayout& l,
               const proto::ProtoObject* a, const proto::ProtoObject* b) {
    return protoClojure::mapEquals(
        ctx, l, a, b, const_cast<void*>(static_cast<const void*>(&l)),
        &eqValues);
}

TEST_F(MapOpsFixture, EqualityIgnoresInsertionOrderBeyondTheSmallForm) {
    // The same 200 entries inserted in ascending and in scrambled order
    // ((i * 73) % 200 is a permutation of 0..199).
    const proto::ProtoObject* ascending = nullptr;
    const proto::ProtoObject* scrambled = nullptr;
    for (long long i = 0; i < 200; ++i) {
        long long key = (i * 73) % 200;
        ascending = assoc(ascending, num(i), num(i * 10));
        scrambled = assoc(scrambled, num(key), num(key * 10));
    }
    EXPECT_EQ(protoClojure::mapCount(ctx, layout, ascending), 200u);
    EXPECT_EQ(protoClojure::mapCount(ctx, layout, scrambled), 200u);
    EXPECT_TRUE(mapsEqual(ctx, layout, ascending, scrambled));
    EXPECT_TRUE(mapsEqual(ctx, layout, scrambled, ascending));

    // One different value breaks equality in both directions.
    const proto::ProtoObject* changed = assoc(scrambled, num(117), num(0));
    EXPECT_FALSE(mapsEqual(ctx, layout, ascending, changed));
    EXPECT_FALSE(mapsEqual(ctx, layout, changed, ascending));
}

TEST_F(MapOpsFixture, EqualityRequiresTheSameKeysAndSize) {
    const proto::ProtoObject* ab = assoc(assoc(nullptr, kw(":a"), num(1)),
                                         kw(":b"), num(2));
    const proto::ProtoObject* ac = assoc(assoc(nullptr, kw(":a"), num(1)),
                                         kw(":c"), num(2));
    const proto::ProtoObject* a = assoc(nullptr, kw(":a"), num(1));
    EXPECT_FALSE(mapsEqual(ctx, layout, ab, ac));
    EXPECT_FALSE(mapsEqual(ctx, layout, ab, a));
    EXPECT_FALSE(mapsEqual(ctx, layout, a, ab));

    // nullptr, a map built from no pairs and a map emptied by dissoc are
    // all the empty map.
    const proto::ProtoObject* built =
        protoClojure::mapAssocPairs(ctx, layout, nullptr,
                                    static_cast<const proto::ProtoObject* const*>(nullptr), 0);
    const proto::ProtoObject* emptied =
        protoClojure::mapDissoc(ctx, layout, a, kw(":a"));
    EXPECT_EQ(protoClojure::mapCount(ctx, layout, built), 0u);
    EXPECT_EQ(protoClojure::mapCount(ctx, layout, emptied), 0u);
    EXPECT_TRUE(mapsEqual(ctx, layout, nullptr, built));
    EXPECT_TRUE(mapsEqual(ctx, layout, built, emptied));
    EXPECT_FALSE(mapsEqual(ctx, layout, nullptr, a));
    EXPECT_FALSE(mapsEqual(ctx, layout, a, emptied));
}

TEST_F(MapOpsFixture, EqualityMatchesKeysInsideCollidingBuckets) {
    MapLayout colliding{marker, stateKey,
        [](proto::ProtoContext*, const proto::ProtoObject*) -> unsigned long {
            return 42;
        }};
    auto build = [&](std::initializer_list<std::pair<const char*, long long>> kvs) {
        const proto::ProtoObject* m = nullptr;
        for (const auto& [k, v] : kvs) {
            const proto::ProtoObject* kv[2] = {kw(k), num(v)};
            m = protoClojure::mapAssocPairs(ctx, colliding, m, kv, 2);
        }
        return m;
    };
    // One bucket, triples in a different order.
    EXPECT_TRUE(mapsEqual(ctx, colliding,
                          build({{":x1", 1}, {":x2", 2}, {":x3", 3}}),
                          build({{":x3", 3}, {":x1", 1}, {":x2", 2}})));
    EXPECT_FALSE(mapsEqual(ctx, colliding,
                           build({{":x1", 1}, {":x2", 2}, {":x3", 3}}),
                           build({{":x3", 3}, {":x1", 1}, {":x2", 9}})));
    EXPECT_FALSE(mapsEqual(ctx, colliding,
                           build({{":x1", 1}, {":x2", 2}}),
                           build({{":x1", 1}, {":x4", 2}})));
}

TEST_F(MapOpsFixture, EqualityComparesValuesThroughTheCallback) {
    const proto::ProtoObject* inner1 = assoc(assoc(nullptr, kw(":x"), num(1)),
                                             kw(":y"), num(2));
    const proto::ProtoObject* inner2 = assoc(assoc(nullptr, kw(":y"), num(2)),
                                             kw(":x"), num(1));
    const proto::ProtoObject* inner3 = assoc(assoc(nullptr, kw(":y"), num(3)),
                                             kw(":x"), num(1));
    const proto::ProtoObject* outer1 = assoc(nullptr, kw(":m"), inner1);
    const proto::ProtoObject* outer2 = assoc(nullptr, kw(":m"), inner2);
    const proto::ProtoObject* outer3 = assoc(nullptr, kw(":m"), inner3);
    // Distinct inner map objects, equal only by value.
    EXPECT_NE(inner1, inner2);
    EXPECT_TRUE(mapsEqual(ctx, layout, outer1, outer2));
    EXPECT_FALSE(mapsEqual(ctx, layout, outer1, outer3));

    // A comparator that rejects everything makes non-empty maps unequal,
    // except a map compared with itself.
    auto never = [](proto::ProtoContext*, void*, const proto::ProtoObject*,
                    const proto::ProtoObject*) { return false; };
    EXPECT_FALSE(protoClojure::mapEquals(ctx, layout, inner1, inner2, nullptr, never));
    EXPECT_TRUE(protoClojure::mapEquals(ctx, layout, inner1, inner1, nullptr, never));
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

TEST_F(MapOpsFixture, MapKeysMatchByValue) {
    // Two equal maps built in different insertion orders: distinct objects.
    const proto::ProtoObject* k1 = assoc(assoc(nullptr, kw(":a"), num(1)),
                                         kw(":b"), num(2));
    const proto::ProtoObject* k2 = assoc(assoc(nullptr, kw(":b"), num(2)),
                                         kw(":a"), num(1));
    ASSERT_NE(k1, k2);
    const proto::ProtoObject* outer = assoc(nullptr, k1, kw(":x"));
    bool found = false;
    EXPECT_EQ(protoClojure::mapGet(ctx, layout, outer, k2, &found), kw(":x"));
    EXPECT_TRUE(found);

    // assoc under an equal key replaces the value and keeps the stored key;
    // dissoc under an equal key removes the entry.
    auto e = walk(assoc(outer, k2, kw(":y")));
    ASSERT_EQ(e.size(), 2u);
    EXPECT_EQ(e[0], k1);
    EXPECT_EQ(e[1], kw(":y"));
    EXPECT_EQ(protoClojure::mapCount(
                  ctx, layout, protoClojure::mapDissoc(ctx, layout, outer, k2)),
              0u);
}

TEST_F(MapOpsFixture, SequentialKeysMatchAcrossListsAndTuples) {
    // 200-element keys, past the small inline forms.
    const proto::ProtoList* l = ctx->newList();
    for (long long i = 0; i < 200; ++i) l = l->appendLast(ctx, num(i));
    const proto::ProtoObject* listKey = l->asObject(ctx);
    const proto::ProtoObject* tupleKey = ctx->newTupleFromList(l)->asObject(ctx);
    const proto::ProtoObject* m = assoc(nullptr, tupleKey, kw(":v"));
    bool found = false;
    EXPECT_EQ(protoClojure::mapGet(ctx, layout, m, listKey, &found), kw(":v"));
    EXPECT_TRUE(found);
    protoClojure::mapGet(ctx, layout, m, l->removeLast(ctx)->asObject(ctx), &found);
    EXPECT_FALSE(found);
}

TEST_F(MapOpsFixture, NumericKeysMatchAcrossTypes) {
    // Deviation D15: numbers are = across types, so equal numbers must find
    // each other as keys whatever their representation.
    const proto::ProtoObject* two35 = num(1LL << 35);
    const proto::ProtoObject* two70 = two35->multiply(ctx, two35);  // LargeInteger
    const proto::ProtoObject* two60 = num(1LL << 60);  // LargeInteger in long range
    struct Case {
        const proto::ProtoObject* stored;
        const proto::ProtoObject* probe;
        bool match;
    };
    const Case cases[] = {
        {num(1), ctx->fromDouble(1.0), true},
        {ctx->fromDouble(-3.0), num(-3), true},
        {num(0), ctx->fromDouble(-0.0), true},
        {two60, ctx->fromDouble(1152921504606846976.0), true},
        {ctx->fromDouble(1180591620717411303424.0), two70, true},
        {ctx->fromDouble(2.5), ctx->fromDouble(2.5), true},
        {two70->add(ctx, num(1)), ctx->fromDouble(1180591620717411303424.0), false},
        {num(1), ctx->fromDouble(1.5), false},
    };
    for (std::size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const proto::ProtoObject* m = assoc(nullptr, cases[i].stored, kw(":v"));
        bool found = false;
        protoClojure::mapGet(ctx, layout, m, cases[i].probe, &found);
        EXPECT_EQ(found, cases[i].match) << "case " << i;
    }
}

TEST_F(MapOpsFixture, MapsWithCollectionKeysCompareByValue) {
    const proto::ProtoObject* k1 = assoc(assoc(nullptr, kw(":a"), num(1)),
                                         kw(":b"), num(2));
    const proto::ProtoObject* k2 = assoc(assoc(nullptr, kw(":b"), num(2)),
                                         kw(":a"), num(1));
    EXPECT_TRUE(mapsEqual(ctx, layout, assoc(nullptr, k1, num(10)),
                          assoc(nullptr, k2, num(10))));
    EXPECT_FALSE(mapsEqual(ctx, layout, assoc(nullptr, k1, num(10)),
                           assoc(nullptr, k2, num(11))));
}

} // namespace
