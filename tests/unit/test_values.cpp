// Unit tests for value equality (valuesEqual in src/runtime/Primitives.h).
// The conformance fixtures under tests/conformance/08-collections and
// tests/conformance/16-maps cover the Clojure-visible behaviour; these
// tests exercise collections built directly with protoCore, past the
// small inline forms of lists and tuples.

#include "runtime/MapOps.h"
#include "runtime/Primitives.h"

#include "protoCore.h"
#include <gtest/gtest.h>

using protoClojure::MapLayout;
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

} // namespace
