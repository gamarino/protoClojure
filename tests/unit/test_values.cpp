// Unit tests for value equality (valuesEqual in src/runtime/Primitives.h)
// and its relation to map key equality (src/runtime/MapOps.h). The
// conformance fixtures under tests/conformance/08-collections and
// tests/conformance/16-maps cover the Clojure-visible behaviour; these tests
// exercise collections built directly with protoCore, past the small inline
// forms of lists and tuples.

#include "runtime/MapOps.h"
#include "runtime/Named.h"
#include "runtime/Primitives.h"

#include "protoCore.h"
#include <gtest/gtest.h>

#include <climits>
#include <cmath>
#include <cstddef>

using protoClojure::valueTypeName;
using protoClojure::valuesEqual;

namespace {

struct ValuesFixture : ::testing::Test {
    proto::ProtoSpace space;
    proto::ProtoContext* ctx = space.rootContext;
    // The Named intern table is mutated and stays mutable.
    protoClojure::NamedLayout named{
        space.objectPrototype->newChild(ctx),
        space.objectPrototype->newChild(ctx, /*isMutable=*/true),
        proto::ProtoString::createSymbol(ctx, "__spelling__")};

    // The interned keyword (or, without a leading colon, symbol) `spelling`.
    const proto::ProtoObject* kw(const char* spelling) const {
        return protoClojure::internNamed(ctx, named, spelling);
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
    const proto::ProtoObject* assoc(const proto::ProtoObject* m, const proto::ProtoObject* k,
                                    const proto::ProtoObject* v) const {
        const proto::ProtoObject* kv[2] = {k, v};
        return protoClojure::mapAssocPairs(ctx, m, kv, 2);
    }
    bool eq(const proto::ProtoObject* a, const proto::ProtoObject* b) const {
        return valuesEqual(ctx, a, b);
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
    const proto::ProtoObject* m = assoc(nullptr, kw(":a"), num(1));
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

TEST_F(ValuesFixture, KeywordsAndSymbolsAreInternedAndNeverEqualStrings) {
    // protoCore stores short ASCII strings inline, so a symbol and a string
    // spelled ":a" are one tagged pointer; longer spellings compare equal by
    // content. Keywords and symbols must differ from strings in every case.
    for (const char* spelling :
         {":a", ":a-much-longer-keyword", ":\xC3\xB1" "and\xC3\xBA", "a",
          "a-much-longer-symbol"}) {
        const proto::ProtoObject* value = kw(spelling);
        const proto::ProtoObject* string = ctx->fromUTF8String(spelling);
        EXPECT_EQ(value, kw(spelling)) << spelling;
        EXPECT_TRUE(protoClojure::isNamed(ctx, named, value)) << spelling;
        EXPECT_FALSE(protoClojure::isNamed(ctx, named, string)) << spelling;
        EXPECT_EQ(protoClojure::namedSpelling(ctx, named, value)->toStdString(ctx),
                  spelling);
        EXPECT_TRUE(eq(value, kw(spelling))) << spelling;
        EXPECT_FALSE(eq(value, string)) << spelling;
        EXPECT_FALSE(eq(string, value)) << spelling;
    }
    EXPECT_FALSE(eq(kw(":a"), kw("a")));

    // As map keys, a keyword and the string of its spelling are two entries.
    const proto::ProtoObject* kv[4] = {
        kw(":a"), num(1), ctx->fromUTF8String(":a"), num(2)};
    const proto::ProtoObject* m =
        protoClojure::mapAssocPairs(ctx, nullptr, kv, 4);
    EXPECT_EQ(protoClojure::mapCount(ctx, m), 2u);
    bool found = false;
    EXPECT_EQ(protoClojure::mapGet(ctx, m, kw(":a"), &found), num(1));
    EXPECT_TRUE(found);
    EXPECT_EQ(protoClojure::mapGet(ctx, m, ctx->fromUTF8String(":a"), &found),
              num(2));
    EXPECT_TRUE(found);
    protoClojure::mapGet(ctx, m, kw("a"), &found);
    EXPECT_FALSE(found);
}

TEST_F(ValuesFixture, SequentialElementsUseNumericCrossTypeEquality) {
    // Deviation D15: (= 1 1.0) is true, and so elements compare that way.
    const proto::ProtoObject* ints = obj(list(1, 2));
    const proto::ProtoObject* floats = tuple(ctx->newList()
        ->appendLast(ctx, ctx->fromDouble(1.0))
        ->appendLast(ctx, ctx->fromDouble(2.0)));
    EXPECT_TRUE(eq(ints, floats));
}

TEST_F(ValuesFixture, NaNIsEqualOnlyToItself) {
    // `=` follows IEEE for NaN (protoCore partialCompare): a NaN is equal to
    // no number and to no other NaN object, and a NaN object is equal to
    // itself through the identity test, as in JVM Clojure's Util.equiv.
    auto dbl = [&](double d) { return ctx->fromDouble(d); };
    const proto::ProtoObject* nan = ctx->fromDouble(std::nan(""));
    const proto::ProtoObject* otherNan = ctx->fromDouble(-std::nan(""));
    EXPECT_TRUE(eq(nan, nan));
    EXPECT_FALSE(eq(nan, otherNan));
    EXPECT_FALSE(eq(nan, num(0)));
    EXPECT_FALSE(eq(num(0), nan));
    EXPECT_FALSE(eq(nan, dbl(0.0)));
    EXPECT_FALSE(eq(nan, dbl(-0.0)));
    EXPECT_FALSE(eq(nan, num(1)));
    EXPECT_FALSE(eq(nan, dbl(HUGE_VAL)));
    // -0.0 stays equal to 0.0 and to 0.
    EXPECT_TRUE(eq(dbl(-0.0), dbl(0.0)));
    EXPECT_TRUE(eq(dbl(-0.0), num(0)));
}

TEST_F(ValuesFixture, EqualValuesAreOneMapKeyExceptAcrossNumericTypes) {
    // Every value carries a group number: two values must name one map entry
    // exactly when their groups are equal. Values equal
    // under `=` share a group, except numbers of different types (1 and 1.0,
    // D15), which are different keys at every depth of a collection key.
    const proto::ProtoObject* list12 = obj(list(1, 2));
    const proto::ProtoObject* floats12 = obj(ctx->newList()
        ->appendLast(ctx, ctx->fromDouble(1.0))->appendLast(ctx, ctx->fromDouble(2.0)));
    const proto::ProtoObject* longString = ctx->fromUTF8String("a-long-string-key");
    const proto::ProtoObject* builtString = reinterpret_cast<const proto::ProtoObject*>(
        reinterpret_cast<const proto::ProtoString*>(ctx->fromUTF8String("a-long-"))
            ->appendLast(ctx, reinterpret_cast<const proto::ProtoString*>(
                                  ctx->fromUTF8String("string-key"))));
    struct Entry {
        const proto::ProtoObject* value;
        int group;
    };
    const Entry pool[] = {
        {num(1), 1},
        {ctx->fromDouble(1.0), 2},
        {num(2), 3},
        {PROTO_NONE, 4},
        {PROTO_TRUE, 5},
        {kw(":a"), 6},
        {ctx->fromUTF8String(":a"), 7},
        {ctx->fromUTF8String("ab"), 8},
        {longString, 9},
        {builtString, 9},
        {list12, 10},
        {tuple(list(1, 2)), 10},
        {floats12, 11},
        {tuple(floats12->asList(ctx)), 11},
        {obj(ctx->newList()), 12},
        {ctx->newTuple()->asObject(ctx), 12},
        {assoc(nullptr, num(1), list12), 13},
        {assoc(nullptr, num(1), tuple(list(1, 2))), 13},
        {assoc(nullptr, ctx->fromDouble(1.0), tuple(list(1, 2))), 14},
        {assoc(nullptr, num(1), floats12), 15},
        {assoc(assoc(nullptr, kw(":a"), num(1)), list12, kw(":b")), 16},
        {assoc(assoc(nullptr, tuple(list(1, 2)), kw(":b")), kw(":a"), num(1)), 16},
    };
    const std::size_t n = sizeof(pool) / sizeof(pool[0]);
    std::size_t equalAcrossKeys = 0;
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) {
            const bool sameKey =
                protoClojure::keyEquals(ctx, pool[i].value, pool[j].value);
            EXPECT_EQ(sameKey, pool[i].group == pool[j].group)
                << "pool[" << i << "] and pool[" << j << "]";
            if (sameKey) {
                // Keys that name one entry must hash alike, or the entry is
                // in a slot the lookup never reaches.
                EXPECT_EQ(protoClojure::keyHash(ctx, pool[i].value),
                          protoClojure::keyHash(ctx, pool[j].value))
                    << "pool[" << i << "] and pool[" << j << "]";
                // One key implies `=`.
                EXPECT_TRUE(eq(pool[i].value, pool[j].value))
                    << "pool[" << i << "] and pool[" << j << "]";
            }
            if (!sameKey && eq(pool[i].value, pool[j].value)) ++equalAcrossKeys;
        }
    }
    // Equal under `=` but different keys, counted in both directions:
    // 1 and 1.0 (2 pairs), the two integer and the two float sequences of
    // 1 2 (8 pairs), and {1 (1 2)} / {1 [1 2]} against {1 (1.0 2.0)} (4 pairs).
    EXPECT_EQ(equalAcrossKeys, 14u);
}

// The type names the ClassCastException analogue reports
// (throwNotANumber). Without an installed ActiveCallContext, runtime objects
// such as keywords are "an object"; maps are recognised by their tag.
TEST_F(ValuesFixture, ValueTypeNamesForErrorMessages) {
    EXPECT_STREQ(valueTypeName(ctx, nullptr), "nil");
    EXPECT_STREQ(valueTypeName(ctx, PROTO_NONE), "nil");
    EXPECT_STREQ(valueTypeName(ctx, PROTO_TRUE), "a boolean");
    EXPECT_STREQ(valueTypeName(ctx, PROTO_FALSE), "a boolean");
    EXPECT_STREQ(valueTypeName(ctx, num(7)), "an integer");
    EXPECT_STREQ(valueTypeName(ctx, ctx->fromString("123456789012345678901234567890", 10)),
                 "an integer");
    EXPECT_STREQ(valueTypeName(ctx, ctx->fromDouble(1.5)), "a float");
    EXPECT_STREQ(valueTypeName(ctx, ctx->fromUTF8String("ab")), "a string");
    EXPECT_STREQ(valueTypeName(ctx, ctx->fromUTF8String("a longer string")), "a string");
    EXPECT_STREQ(valueTypeName(ctx, obj(list(0, 3))), "a list");
    EXPECT_STREQ(valueTypeName(ctx, obj(list(0, 300))), "a list");
    EXPECT_STREQ(valueTypeName(ctx, tuple(list(0, 3))), "a vector");
    EXPECT_STREQ(valueTypeName(ctx, assoc(nullptr, num(1), num(2))), "a map");
    EXPECT_STREQ(valueTypeName(ctx, space.objectPrototype->newChild(ctx)), "an object");

    EXPECT_TRUE(protoClojure::isNumber(ctx, num(7)));
    EXPECT_TRUE(protoClojure::isNumber(ctx, ctx->fromDouble(0.5)));
    EXPECT_FALSE(protoClojure::isNumber(ctx, nullptr));
    EXPECT_FALSE(protoClojure::isNumber(ctx, PROTO_NONE));
    EXPECT_FALSE(protoClojure::isNumber(ctx, ctx->fromUTF8String("1")));

    try {
        protoClojure::throwNotANumber(ctx, "*", ctx->fromUTF8String("ab"));
        FAIL() << "throwNotANumber returned";
    } catch (const std::runtime_error& e) {
        EXPECT_STREQ(e.what(), "ClassCastException: * expects a number, got a string");
    }
}

} // namespace
