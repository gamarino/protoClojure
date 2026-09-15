// Unit tests for value equality (valuesEqual in src/runtime/Primitives.h).
// The conformance fixtures under tests/conformance/08-collections and
// tests/conformance/16-maps cover the Clojure-visible behaviour; these
// tests exercise collections built directly with protoCore, past the
// small inline forms of lists and tuples.

#include "runtime/MapOps.h"
#include "runtime/Primitives.h"

#include "protoCore.h"
#include <gtest/gtest.h>

#include <climits>
#include <cmath>
#include <cstddef>

using protoClojure::MapLayout;
using protoClojure::valueHash;
using protoClojure::valuesEqual;

namespace {

struct ValuesFixture : ::testing::Test {
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

    // A list (from, from+1, ..., from+n-1), with `changed` replacing the
    // element at `changeAt` when changeAt >= 0.
    const proto::ProtoList* list(long long from, long long n,
                                 long long changeAt = -1,
                                 const proto::ProtoObject* changed = nullptr) const {
        const proto::ProtoList* l = ctx->newList();
        for (long long i = 0; i < n; ++i)
            l = l->appendLast(ctx, i == changeAt ? changed : num(from + i));
        return l;
    }
    const proto::ProtoObject* obj(const proto::ProtoList* l) const {
        return l->asObject(ctx);
    }
    const proto::ProtoObject* tuple(const proto::ProtoList* l) const {
        return ctx->newTupleFromList(l)->asObject(ctx);
    }
    bool eq(const proto::ProtoObject* a, const proto::ProtoObject* b) const {
        return valuesEqual(ctx, layout, a, b);
    }
};

TEST_F(ValuesFixture, ListsAndTuplesCompareElementWiseBeyondTheSmallForm) {
    const proto::ProtoObject* l1 = obj(list(0, 300));
    const proto::ProtoObject* l2 = obj(list(0, 300));
    const proto::ProtoObject* t1 = tuple(list(0, 300));
    ASSERT_NE(l1, l2);
    EXPECT_TRUE(eq(l1, l2));
    EXPECT_TRUE(eq(l1, t1));
    EXPECT_TRUE(eq(t1, l2));

    // One different element (first, middle, last) breaks equality in both
    // directions and for both concrete types.
    for (long long at : {0LL, 150LL, 299LL}) {
        const proto::ProtoObject* dl = obj(list(0, 300, at, num(-1)));
        const proto::ProtoObject* dt = tuple(list(0, 300, at, num(-1)));
        EXPECT_FALSE(eq(l1, dl)) << "at " << at;
        EXPECT_FALSE(eq(dl, l1)) << "at " << at;
        EXPECT_FALSE(eq(t1, dt)) << "at " << at;
        EXPECT_FALSE(eq(dt, l1)) << "at " << at;
    }

    // A prefix is not equal.
    EXPECT_FALSE(eq(l1, obj(list(0, 299))));
    EXPECT_FALSE(eq(tuple(list(0, 299)), l1));
}

TEST_F(ValuesFixture, EmptySequentialCollectionsAreEqual) {
    const proto::ProtoObject* emptyList = obj(ctx->newList());
    const proto::ProtoObject* emptyTuple = ctx->newTuple()->asObject(ctx);
    EXPECT_TRUE(eq(emptyList, emptyTuple));
    EXPECT_TRUE(eq(emptyTuple, emptyList));
    EXPECT_FALSE(eq(emptyList, PROTO_NONE));
    EXPECT_FALSE(eq(nullptr, emptyTuple));
}

TEST_F(ValuesFixture, NestedSequentialCollectionsCompareByValue) {
    // [1 (2 3) [4]] against (1 [2 3] (4)), then with the innermost 4 changed.
    auto build = [&](bool outerList, long long last) {
        const proto::ProtoList* inner23 = list(2, 2);
        const proto::ProtoList* inner4 = list(last, 1);
        const proto::ProtoList* outer = ctx->newList()
            ->appendLast(ctx, num(1))
            ->appendLast(ctx, outerList ? tuple(inner23) : obj(inner23))
            ->appendLast(ctx, outerList ? obj(inner4) : tuple(inner4));
        return outerList ? obj(outer) : tuple(outer);
    };
    EXPECT_TRUE(eq(build(false, 4), build(true, 4)));
    EXPECT_FALSE(eq(build(false, 4), build(true, 5)));
}

TEST_F(ValuesFixture, SequentialNeverEqualsMapsStringsOrNumbers) {
    const proto::ProtoObject* pairs = obj(ctx->newList()
        ->appendLast(ctx, kw(":a"))->appendLast(ctx, num(1)));
    const proto::ProtoObject* kv[2] = {kw(":a"), num(1)};
    const proto::ProtoObject* m =
        protoClojure::mapAssocPairs(ctx, layout, nullptr, kv, 2);
    EXPECT_FALSE(eq(pairs, m));
    EXPECT_FALSE(eq(m, tuple(pairs->asList(ctx))));

    const proto::ProtoObject* ab = ctx->fromUTF8String("ab");
    const proto::ProtoObject* chars = obj(ctx->newList()
        ->appendLast(ctx, ctx->fromUTF8String("a"))
        ->appendLast(ctx, ctx->fromUTF8String("b")));
    EXPECT_FALSE(eq(chars, ab));
    EXPECT_FALSE(eq(ab, chars));
    EXPECT_FALSE(eq(obj(list(1, 1)), num(1)));
    EXPECT_FALSE(eq(num(1), tuple(list(1, 1))));
}

TEST_F(ValuesFixture, SequentialElementsUseNumericCrossTypeEquality) {
    // Deviation D15: (= 1 1.0) is true, and so elements compare that way.
    const proto::ProtoObject* ints = obj(list(1, 2));
    const proto::ProtoObject* floats = tuple(ctx->newList()
        ->appendLast(ctx, ctx->fromDouble(1.0))
        ->appendLast(ctx, ctx->fromDouble(2.0)));
    EXPECT_TRUE(eq(ints, floats));
}

// --- valueHash ----------------------------------------------------------

TEST_F(ValuesFixture, EqualNumbersHashEquallyAcrossRepresentations) {
    auto h = [&](const proto::ProtoObject* v) { return valueHash(ctx, layout, v); };
    auto dbl = [&](double d) { return ctx->fromDouble(d); };
    const proto::ProtoObject* two35 = num(1LL << 35);
    const proto::ProtoObject* two70 = two35->multiply(ctx, two35);
    const proto::ProtoObject* two100 = two70->multiply(ctx, num(1LL << 30));
    // 3 * 2^80 + 2^30: a LargeInteger whose double mantissa spans several
    // 28-bit chunks of the hash loop.
    const proto::ProtoObject* spread =
        two70->multiply(ctx, num(3 * 1024))->add(ctx, num(1LL << 30));
    struct Pair { const proto::ProtoObject* a; const proto::ProtoObject* b; };
    const Pair equalPairs[] = {
        {num(1), dbl(1.0)},
        {num(-7), dbl(-7.0)},
        {num(0), dbl(-0.0)},
        {num((1LL << 60) + 256), dbl(std::ldexp(1.0, 60) + 256.0)},
        {num(LLONG_MIN), dbl(-std::ldexp(1.0, 63))},
        {two70, dbl(std::ldexp(1.0, 70))},
        {num(0)->subtract(ctx, two70), dbl(-std::ldexp(1.0, 70))},
        {two100, dbl(std::ldexp(1.0, 100))},
        {spread, dbl(std::ldexp(3.0, 80) + std::ldexp(1.0, 30))},
        {dbl(0.1), dbl(0.1)},
        {dbl(-2.75), dbl(-2.75)},
        {dbl(1e300), dbl(1e300)},
        {dbl(HUGE_VAL), dbl(HUGE_VAL)},
    };
    for (std::size_t i = 0; i < sizeof(equalPairs) / sizeof(equalPairs[0]); ++i) {
        ASSERT_TRUE(eq(equalPairs[i].a, equalPairs[i].b)) << "pair " << i;
        EXPECT_EQ(h(equalPairs[i].a), h(equalPairs[i].b)) << "pair " << i;
    }
    // Not required by the contract, but a constant hash would pass the loop
    // above: distinct values must spread.
    EXPECT_NE(h(num(1)), h(num(2)));
    EXPECT_NE(h(num(1)), h(num(-1)));
    EXPECT_NE(h(two70), h(two70->add(ctx, num(1))));
    EXPECT_NE(h(dbl(0.5)), h(dbl(0.25)));
    EXPECT_NE(h(dbl(HUGE_VAL)), h(dbl(-HUGE_VAL)));
}

TEST_F(ValuesFixture, NaNHashesLikeZero) {
    // protoCore compare reports NaN equal to every number, so no consistent
    // hash exists; the documented choice is the hash of 0.
    const proto::ProtoObject* nan = ctx->fromDouble(std::nan(""));
    EXPECT_EQ(valueHash(ctx, layout, nan), valueHash(ctx, layout, num(0)));
}

TEST_F(ValuesFixture, SequentialHashIsSharedByListsAndTuplesAndOrdered) {
    auto h = [&](const proto::ProtoObject* v) { return valueHash(ctx, layout, v); };
    EXPECT_EQ(h(obj(list(0, 300))), h(tuple(list(0, 300))));
    EXPECT_EQ(h(obj(ctx->newList())), h(ctx->newTuple()->asObject(ctx)));
    EXPECT_NE(h(obj(list(1, 2))), h(obj(ctx->newList()
        ->appendLast(ctx, num(2))->appendLast(ctx, num(1)))));
    EXPECT_NE(h(obj(list(0, 300))), h(obj(list(0, 299))));

    const proto::ProtoObject* floats = tuple(ctx->newList()
        ->appendLast(ctx, ctx->fromDouble(1.0))
        ->appendLast(ctx, ctx->fromDouble(2.0)));
    EXPECT_EQ(h(obj(list(1, 2))), h(floats));
}

TEST_F(ValuesFixture, MapHashIgnoresInsertionOrder) {
    auto h = [&](const proto::ProtoObject* v) { return valueHash(ctx, layout, v); };
    auto assoc = [&](const proto::ProtoObject* m, const proto::ProtoObject* k,
                     const proto::ProtoObject* v) {
        const proto::ProtoObject* kv[2] = {k, v};
        return protoClojure::mapAssocPairs(ctx, layout, m, kv, 2);
    };
    // 200 entries with vector values, inserted in ascending and scrambled
    // order ((i * 73) % 200 is a permutation of 0..199).
    const proto::ProtoObject* ascending = nullptr;
    const proto::ProtoObject* scrambled = nullptr;
    for (long long i = 0; i < 200; ++i) {
        long long k = (i * 73) % 200;
        ascending = assoc(ascending, num(i), tuple(list(i, 2)));
        scrambled = assoc(scrambled, num(k), obj(list(k, 2)));
    }
    ASSERT_TRUE(eq(ascending, scrambled));
    EXPECT_EQ(h(ascending), h(scrambled));
    EXPECT_NE(h(ascending), h(assoc(scrambled, num(117), num(0))));

    // Keys and values are not interchangeable: {1 2} and {2 1} differ.
    EXPECT_NE(h(assoc(nullptr, num(1), num(2))), h(assoc(nullptr, num(2), num(1))));
    // The empty map built from no pairs and a map emptied by dissoc.
    const proto::ProtoObject* built = protoClojure::mapAssocPairs(
        ctx, layout, nullptr, static_cast<const proto::ProtoObject* const*>(nullptr), 0);
    const proto::ProtoObject* emptied =
        protoClojure::mapDissoc(ctx, layout, assoc(nullptr, kw(":a"), num(1)), kw(":a"));
    EXPECT_EQ(h(built), h(emptied));
}

TEST_F(ValuesFixture, EqualValuesHashEqually) {
    // Every pair of values in the pool that valuesEqual accepts must hash
    // equally: numbers of both types, strings and keywords, lists, vectors
    // and maps, nested.
    auto assoc = [&](const proto::ProtoObject* m, const proto::ProtoObject* k,
                     const proto::ProtoObject* v) {
        const proto::ProtoObject* kv[2] = {k, v};
        return protoClojure::mapAssocPairs(ctx, layout, m, kv, 2);
    };
    const proto::ProtoObject* list12 = obj(list(1, 2));
    const proto::ProtoObject* floats12 = obj(ctx->newList()
        ->appendLast(ctx, ctx->fromDouble(1.0))->appendLast(ctx, ctx->fromDouble(2.0)));
    const proto::ProtoObject* pool[] = {
        num(1), ctx->fromDouble(1.0), num(2), PROTO_NONE, PROTO_TRUE,
        kw(":a"), ctx->fromUTF8String(":a"), ctx->fromUTF8String("ab"),
        list12, tuple(list(1, 2)), floats12, tuple(floats12->asList(ctx)),
        obj(ctx->newList()), ctx->newTuple()->asObject(ctx),
        assoc(nullptr, num(1), list12),
        assoc(nullptr, ctx->fromDouble(1.0), tuple(list(1, 2))),
        assoc(assoc(nullptr, kw(":a"), num(1)), list12, kw(":b")),
        assoc(assoc(nullptr, tuple(floats12->asList(ctx)), kw(":b")),
              ctx->fromUTF8String(":a"), ctx->fromDouble(1.0)),
    };
    const std::size_t n = sizeof(pool) / sizeof(pool[0]);
    std::size_t equalPairs = 0;
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) {
            if (!eq(pool[i], pool[j])) continue;
            ++equalPairs;
            EXPECT_EQ(valueHash(ctx, layout, pool[i]),
                      valueHash(ctx, layout, pool[j]))
                << "pool[" << i << "] and pool[" << j << "]";
        }
    }
    // Reflexive pairs plus the cross-type ones: the pool is not trivially
    // all-distinct.
    EXPECT_GT(equalPairs, n + 10);
}

} // namespace
