// Unit tests for the runtime map (src/runtime/MapOps.h): canonical keys,
// persistence, equality and walks over maps larger than protoCore's small
// sparse-list form. The conformance fixtures under tests/conformance/16-maps
// cover the Clojure-visible behaviour.

#include "runtime/MapOps.h"
#include "runtime/Named.h"

#include "protoCore.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <set>
#include <utility>
#include <vector>

using protoClojure::MapKeyMarkers;

namespace {

struct MapOpsFixture : ::testing::Test {
    proto::ProtoSpace space;
    proto::ProtoContext* ctx = space.rootContext;
    // Private immutable markers, created as src/main.cpp creates them.
    MapKeyMarkers markers{space.objectPrototype->newChild(ctx),
                          space.objectPrototype->newChild(ctx),
                          space.objectPrototype->newChild(ctx)};
    protoClojure::NamedLayout named{
        space.objectPrototype->newChild(ctx),
        space.objectPrototype->newChild(ctx, /*isMutable=*/true),
        proto::ProtoString::createSymbol(ctx, "__spelling__")};

    const proto::ProtoObject* kw(const char* spelling) const {
        return protoClojure::internNamed(ctx, named, spelling);
    }
    const proto::ProtoObject* num(long long v) const { return ctx->fromLong(v); }
    const proto::ProtoObject* dbl(double v) const { return ctx->fromDouble(v); }
    const proto::ProtoObject* dblBits(std::uint64_t bits) const {
        double v = 0;
        std::memcpy(&v, &bits, sizeof v);
        return ctx->fromDouble(v);
    }
    const proto::ProtoObject* str(const char* s) const { return ctx->fromUTF8String(s); }
    const proto::ProtoObject* concat(const char* a, const char* b) const {
        return reinterpret_cast<const proto::ProtoObject*>(
            reinterpret_cast<const proto::ProtoString*>(str(a))->appendLast(
                ctx, reinterpret_cast<const proto::ProtoString*>(str(b))));
    }
    const proto::ProtoObject* big(const char* decimal) const {
        return ctx->fromString(decimal, 10);
    }
    const proto::ProtoObject* list(std::vector<const proto::ProtoObject*> items) const {
        const proto::ProtoList* l = ctx->newList();
        for (const auto* item : items) l = l->appendLast(ctx, item);
        return l->asObject(ctx);
    }
    const proto::ProtoObject* vec(std::vector<const proto::ProtoObject*> items) const {
        return ctx->newTuple(items)->asObject(ctx);
    }

    const proto::ProtoObject* canon(const proto::ProtoObject* key) const {
        return protoClojure::canonicalKey(ctx, markers, key);
    }
    const proto::ProtoObject* assoc(const proto::ProtoObject* m,
                                    const proto::ProtoObject* k,
                                    const proto::ProtoObject* v) const {
        const proto::ProtoObject* kv[2] = {k, v};
        return protoClojure::mapAssocPairs(ctx, markers, m, kv, 2);
    }
    const proto::ProtoObject* get(const proto::ProtoObject* m,
                                  const proto::ProtoObject* k, bool* found) const {
        return protoClojure::mapGet(ctx, markers, m, k, found);
    }
    bool has(const proto::ProtoObject* m, const proto::ProtoObject* k) const {
        bool found = false;
        get(m, k, &found);
        return found;
    }
    std::vector<std::pair<const proto::ProtoObject*, const proto::ProtoObject*>>
    walk(const proto::ProtoObject* m) const {
        std::vector<std::pair<const proto::ProtoObject*, const proto::ProtoObject*>> out;
        protoClojure::mapForEach(ctx, m, &out,
            [](proto::ProtoContext*, void* self, const proto::ProtoObject* k,
               const proto::ProtoObject* v) {
                static_cast<std::vector<std::pair<const proto::ProtoObject*,
                                                  const proto::ProtoObject*>>*>(self)
                    ->emplace_back(k, v);
            });
        return out;
    }
};

// Value comparator for mapEquals: recurses into maps, otherwise protoCore
// compare.
bool eqValues(proto::ProtoContext* ctx, void* self,
              const proto::ProtoObject* a, const proto::ProtoObject* b) {
    if (protoClojure::isMap(a) && protoClojure::isMap(b))
        return protoClojure::mapEquals(ctx, a, b, self, &eqValues);
    return a == b || a->compare(ctx, b) == 0;
}

bool mapsEqual(proto::ProtoContext* ctx, const proto::ProtoObject* a,
               const proto::ProtoObject* b) {
    return protoClojure::mapEquals(ctx, a, b, nullptr, &eqValues);
}

TEST_F(MapOpsFixture, MapsAreSparseListsRecognisedByTag) {
    const proto::ProtoObject* m = assoc(nullptr, kw(":a"), num(1));
    EXPECT_TRUE(protoClojure::isMap(m));
    EXPECT_TRUE(protoClojure::isMap(ctx->newSparseList()->asObject(ctx)));
    EXPECT_FALSE(protoClojure::isMap(nullptr));
    EXPECT_FALSE(protoClojure::isMap(PROTO_NONE));
    EXPECT_FALSE(protoClojure::isMap(list({num(1)})));
    EXPECT_FALSE(protoClojure::isMap(kw(":a")));
}

TEST_F(MapOpsFixture, CountIsTheNumberOfEntriesAndAssocOfAnEqualKeyAddsNone) {
    const proto::ProtoObject* m = nullptr;
    m = assoc(m, kw(":a"), num(1));
    m = assoc(m, kw(":b"), num(2));
    m = assoc(m, kw(":c"), num(3));
    EXPECT_EQ(protoClojure::mapCount(ctx, m), 3u);
    const proto::ProtoObject* m2 = assoc(m, kw(":a"), num(9));
    EXPECT_EQ(protoClojure::mapCount(ctx, m2), 3u);
    bool found = false;
    EXPECT_EQ(get(m2, kw(":a"), &found), num(9));
    EXPECT_TRUE(found);
    EXPECT_EQ(get(m, kw(":a"), &found), num(1));  // persistence
    EXPECT_EQ(protoClojure::mapCount(ctx, nullptr), 0u);
}

TEST_F(MapOpsFixture, AssocOfAnEqualKeyKeepsTheStoredKeyObject) {
    const proto::ProtoObject* vectorKey = vec({num(1), num(2)});
    const proto::ProtoObject* m = assoc(nullptr, vectorKey, kw(":x"));
    const proto::ProtoObject* m2 = assoc(m, list({num(1), num(2)}), kw(":y"));
    auto entries = walk(m2);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].first, vectorKey);
    EXPECT_EQ(entries[0].second, kw(":y"));

    // A long string key stays the string object first stored, not a symbol.
    const proto::ProtoObject* stringKey = str("a-long-string-key");
    auto stringEntries =
        walk(assoc(assoc(nullptr, stringKey, num(1)), concat("a-long-", "string-key"), num(2)));
    ASSERT_EQ(stringEntries.size(), 1u);
    EXPECT_EQ(stringEntries[0].first, stringKey);
    EXPECT_EQ(stringEntries[0].second, num(2));
}

TEST_F(MapOpsFixture, AssocOfAnIdenticalValueReturnsTheSameMap) {
    const proto::ProtoObject* value = list({num(7)});
    const proto::ProtoObject* m = assoc(nullptr, vec({num(1), num(2)}), value);
    EXPECT_EQ(assoc(m, vec({num(1), num(2)}), value), m);
    EXPECT_EQ(assoc(m, list({num(1), num(2)}), value), m);
    EXPECT_NE(assoc(m, list({num(1), num(2)}), list({num(7)})), m);
}

TEST_F(MapOpsFixture, DissocRemovesOnlyThatKeyAndReturnsTheSameMapWhenAbsent) {
    const proto::ProtoObject* m = nullptr;
    for (const char* k : {":d", ":a", ":c", ":b"}) m = assoc(m, kw(k), num(0));
    const proto::ProtoObject* removed = protoClojure::mapDissoc(ctx, markers, m, kw(":a"));
    EXPECT_EQ(protoClojure::mapCount(ctx, removed), 3u);
    EXPECT_FALSE(has(removed, kw(":a")));
    EXPECT_TRUE(has(removed, kw(":b")) && has(removed, kw(":c")) && has(removed, kw(":d")));
    EXPECT_TRUE(has(m, kw(":a")));  // persistence
    EXPECT_EQ(protoClojure::mapDissoc(ctx, markers, m, kw(":zz")), m);
    EXPECT_EQ(protoClojure::mapDissoc(ctx, markers, nullptr, kw(":a")), nullptr);
}

TEST_F(MapOpsFixture, WalkVisitsEveryEntryOnceBeyondTheSmallForm) {
    const proto::ProtoObject* m = nullptr;
    for (long long i = 0; i < 200; ++i) {
        long long key = (i * 73) % 200;
        m = assoc(m, num(key), num(key * 10));
    }
    EXPECT_EQ(protoClojure::mapCount(ctx, m), 200u);
    auto entries = walk(m);
    ASSERT_EQ(entries.size(), 200u);
    std::set<long long> keys;
    for (const auto& [k, v] : entries) {
        keys.insert(k->asLong(ctx));
        EXPECT_EQ(v->asLong(ctx), k->asLong(ctx) * 10);
    }
    EXPECT_EQ(keys.size(), 200u);
    EXPECT_EQ(*keys.begin(), 0);
    EXPECT_EQ(*keys.rbegin(), 199);

    // Removal inside the AVL form.
    m = protoClojure::mapDissoc(ctx, markers, m, num(117));
    EXPECT_EQ(protoClojure::mapCount(ctx, m), 199u);
    EXPECT_FALSE(has(m, num(117)));
    EXPECT_TRUE(has(m, num(118)));
}

TEST_F(MapOpsFixture, RepeatedKeyInOnePairListKeepsTheFirstKeyAndTheLastValue) {
    const proto::ProtoObject* first = list({num(1)});
    const proto::ProtoObject* kv[6] = {first, num(1), kw(":b"), num(2), vec({num(1)}), num(3)};
    const proto::ProtoObject* m = protoClojure::mapAssocPairs(ctx, markers, nullptr, kv, 6);
    EXPECT_EQ(protoClojure::mapCount(ctx, m), 2u);
    for (const auto& [k, v] : walk(m)) {
        if (k == kw(":b")) continue;
        EXPECT_EQ(k, first);
        EXPECT_EQ(v, num(3));
    }
}

// --- canonical keys: one test per rule -----------------------------------

TEST_F(MapOpsFixture, RuntimeBuiltStringsFindLiteralKeys) {
    // Long strings, short ASCII (inline) strings, and short non-ASCII strings,
    // which protoCore stores inline or as a string cell depending on the
    // operation that produced them.
    EXPECT_EQ(canon(str("a-long-string-key")), canon(concat("a-long-", "string-key")));
    EXPECT_EQ(canon(str("ab")), canon(concat("a", "b")));
    const proto::ProtoObject* slice = reinterpret_cast<const proto::ProtoObject*>(
        reinterpret_cast<const proto::ProtoString*>(str("a\xC3\xB1o"))->getSlice(ctx, 1, 2));
    EXPECT_EQ(canon(str("\xC3\xB1")), canon(slice));
    EXPECT_NE(canon(str("a-long-string-key")), canon(str("a-long-string-kez")));

    const proto::ProtoObject* m = assoc(nullptr, str("a-long-string-key"), num(1));
    bool found = false;
    EXPECT_EQ(get(m, concat("a-long-", "string-key"), &found), num(1));
    EXPECT_TRUE(found);
}

TEST_F(MapOpsFixture, EqualVectorsAndListsAreOneKey) {
    const proto::ProtoObject* vector12 = vec({num(1), num(2)});
    EXPECT_EQ(canon(vector12), vector12);  // already canonical: no rebuild
    EXPECT_EQ(canon(list({num(1), num(2)})), vector12);
    EXPECT_EQ(canon(list({})), canon(vec({})));
    EXPECT_NE(canon(list({num(2), num(1)})), vector12);

    // 300 elements, past the small forms of lists and tuples.
    std::vector<const proto::ProtoObject*> items;
    for (long long i = 0; i < 300; ++i) items.push_back(num(i));
    EXPECT_EQ(canon(list(items)), canon(vec(items)));
}

TEST_F(MapOpsFixture, NestedNonCanonicalElementsAreCanonicalized) {
    const proto::ProtoObject* stored =
        vec({vec({str("a-long-string-key")}), dbl(1.5), list({num(1), vec({num(2)})})});
    const proto::ProtoObject* probe =
        list({list({concat("a-long-", "string-key")}), ctx->fromDouble(3.0 / 2.0),
              vec({num(1), list({num(2)})})});
    EXPECT_NE(canon(stored), stored);  // rebuilt: a string and a double inside
    EXPECT_EQ(canon(stored), canon(probe));
    EXPECT_NE(canon(stored),
              canon(list({list({concat("a-long-", "string-key")}), dbl(1.5),
                          vec({num(1), list({num(3)})})})));
}

TEST_F(MapOpsFixture, DoublesKeyByBitPattern) {
    EXPECT_EQ(canon(dbl(1.5)), canon(ctx->fromDouble(3.0 / 2.0)));
    EXPECT_NE(canon(dbl(0.0)), canon(dbl(-0.0)));
    const proto::ProtoObject* quietNaN = dbl(std::numeric_limits<double>::quiet_NaN());
    EXPECT_EQ(canon(quietNaN), canon(dblBits(0x7FF8000000000000ULL)));
    // The default NaN x86-64 arithmetic produces (0.0 / 0.0) has the sign bit
    // set: a different key.
    EXPECT_NE(canon(dblBits(0x7FF8000000000000ULL)), canon(dblBits(0xFFF8000000000000ULL)));

    const proto::ProtoObject* m = assoc(nullptr, quietNaN, kw(":nan"));
    bool found = false;
    EXPECT_EQ(get(m, dbl(std::numeric_limits<double>::quiet_NaN()), &found), kw(":nan"));
    EXPECT_TRUE(found);
    get(m, dblBits(0xFFF8000000000000ULL), &found);
    EXPECT_FALSE(found);
}

TEST_F(MapOpsFixture, BigIntegersBuiltTwoWaysAreOneKey) {
    const proto::ProtoObject* two35 = num(1LL << 35);
    const proto::ProtoObject* two70 = two35->multiply(ctx, two35);
    EXPECT_EQ(canon(two70), canon(big("1180591620717411303424")));
    EXPECT_NE(canon(two70), canon(two70->add(ctx, num(1))));
    EXPECT_NE(canon(two70), canon(num(0)->subtract(ctx, two70)));
    EXPECT_EQ(canon(num(0)->subtract(ctx, two70)), canon(big("-1180591620717411303424")));

    // 2^208: its hexadecimal digits are a 1 followed by 52 zeros, four whole
    // 13-digit limbs of zeros above the least significant end.
    const proto::ProtoObject* two104 = two70->multiply(ctx, num(1LL << 34));
    EXPECT_EQ(canon(two104->multiply(ctx, two104)),
              canon(big("411376139330301510538742295639337626245683966408394965837152256")));

    // protoCore never stores a SmallInteger-range value as a LargeInteger, so
    // an integer computed through big intermediates is the SmallInteger key.
    const proto::ProtoObject* back = two70->subtract(ctx, two70->subtract(ctx, num(5)));
    EXPECT_TRUE(proto::isSmallInt(back));
    EXPECT_EQ(canon(back), num(5));
}

TEST_F(MapOpsFixture, NumericKeysAreDistinctAcrossTypes) {
    const proto::ProtoObject* two35 = num(1LL << 35);
    const proto::ProtoObject* two70 = two35->multiply(ctx, two35);  // LargeInteger
    const proto::ProtoObject* two60 = num(1LL << 60);               // LargeInteger
    struct Case {
        const proto::ProtoObject* stored;
        const proto::ProtoObject* probe;
        bool match;
    };
    const Case cases[] = {
        {num(1), dbl(1.0), false},
        {dbl(-3.0), num(-3), false},
        {num(0), dbl(-0.0), false},
        {num(0), dbl(0.0), false},
        {two60, dbl(1152921504606846976.0), false},
        {dbl(1180591620717411303424.0), two70, false},
        {dbl(2.5), dbl(2.5), true},
        {two60, num(1LL << 30)->multiply(ctx, num(1LL << 30)), true},
        {num(1), num(1), true},
    };
    for (std::size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const proto::ProtoObject* m = assoc(nullptr, cases[i].stored, kw(":v"));
        EXPECT_EQ(has(m, cases[i].probe), cases[i].match) << "case " << i;
    }
}

TEST_F(MapOpsFixture, MapsAsKeysMatchByValueWhateverTheInsertionOrder) {
    const proto::ProtoObject* ascending = nullptr;
    const proto::ProtoObject* scrambled = nullptr;
    for (long long i = 0; i < 20; ++i) {
        long long k = (i * 7) % 20;
        ascending = assoc(ascending, num(i), vec({num(i)}));
        scrambled = assoc(scrambled, num(k), list({num(k)}));  // equal values
    }
    ASSERT_NE(ascending, scrambled);
    EXPECT_EQ(canon(ascending), canon(scrambled));
    EXPECT_NE(canon(ascending), canon(assoc(scrambled, num(7), list({num(8)}))));
    EXPECT_NE(canon(assoc(nullptr, kw(":a"), num(1))), canon(assoc(nullptr, kw(":a"), dbl(1.0))));

    const proto::ProtoObject* outer = assoc(nullptr, ascending, kw(":x"));
    bool found = false;
    EXPECT_EQ(get(outer, scrambled, &found), kw(":x"));
    EXPECT_TRUE(found);
}

TEST_F(MapOpsFixture, KeywordsAndStringsWithTheSameSpellingAreDifferentKeys) {
    for (const char* spelling : {":a", ":a-much-longer-keyword"}) {
        EXPECT_NE(canon(kw(spelling)), canon(str(spelling))) << spelling;
        EXPECT_EQ(canon(kw(spelling)), kw(spelling)) << spelling;
    }
}

TEST_F(MapOpsFixture, MarkerTuplesNeverCollideWithUserVectors) {
    // A user vector spelling a marker tuple with the keyword of the same name
    // is its own key: the markers are private objects, not keywords.
    std::uint64_t bits = 0;
    const double one = 1.0;
    std::memcpy(&bits, &one, sizeof bits);
    const proto::ProtoObject* userDouble =
        vec({kw(":double"), num(static_cast<long long>(bits >> 32)),
             num(static_cast<long long>(bits & 0xFFFFFFFFULL))});
    EXPECT_NE(canon(dbl(1.0)), canon(userDouble));
    EXPECT_NE(canon(assoc(nullptr, kw(":a"), num(1))), canon(vec({kw(":map"), kw(":a"), num(1)})));
    const proto::ProtoObject* two35 = num(1LL << 35);
    EXPECT_NE(canon(two35->multiply(ctx, two35)),
              canon(vec({kw(":bigint"), num(0), num(0), num(4096)})));
}

TEST_F(MapOpsFixture, DerivedMapsAreIndependent) {
    const proto::ProtoObject* base = assoc(nullptr, vec({num(1)}), kw(":base"));
    const proto::ProtoObject* left = assoc(base, list({num(2)}), kw(":left"));
    const proto::ProtoObject* right = assoc(base, list({num(1)}), kw(":right"));
    bool found = false;
    EXPECT_EQ(get(base, list({num(1)}), &found), kw(":base"));
    EXPECT_FALSE(has(base, vec({num(2)})));
    EXPECT_EQ(get(left, vec({num(1)}), &found), kw(":base"));
    EXPECT_TRUE(has(left, vec({num(2)})));
    EXPECT_EQ(get(right, vec({num(1)}), &found), kw(":right"));
    EXPECT_FALSE(has(right, vec({num(2)})));
    EXPECT_EQ(protoClojure::mapCount(ctx, right), 1u);
}

// --- equality --------------------------------------------------------------

TEST_F(MapOpsFixture, EqualityIgnoresInsertionOrderBeyondTheSmallForm) {
    const proto::ProtoObject* ascending = nullptr;
    const proto::ProtoObject* scrambled = nullptr;
    for (long long i = 0; i < 200; ++i) {
        long long key = (i * 73) % 200;
        ascending = assoc(ascending, num(i), num(i * 10));
        scrambled = assoc(scrambled, num(key), num(key * 10));
    }
    EXPECT_TRUE(mapsEqual(ctx, ascending, scrambled));
    EXPECT_TRUE(mapsEqual(ctx, scrambled, ascending));
    const proto::ProtoObject* changed = assoc(scrambled, num(117), num(0));
    EXPECT_FALSE(mapsEqual(ctx, ascending, changed));
    EXPECT_FALSE(mapsEqual(ctx, changed, ascending));
}

TEST_F(MapOpsFixture, EqualityRequiresTheSameKeysAndSize) {
    const proto::ProtoObject* ab = assoc(assoc(nullptr, kw(":a"), num(1)), kw(":b"), num(2));
    const proto::ProtoObject* ac = assoc(assoc(nullptr, kw(":a"), num(1)), kw(":c"), num(2));
    const proto::ProtoObject* a = assoc(nullptr, kw(":a"), num(1));
    EXPECT_FALSE(mapsEqual(ctx, ab, ac));
    EXPECT_FALSE(mapsEqual(ctx, ab, a));
    EXPECT_FALSE(mapsEqual(ctx, a, ab));

    const proto::ProtoObject* built = protoClojure::mapAssocPairs(
        ctx, markers, nullptr, static_cast<const proto::ProtoObject* const*>(nullptr), 0);
    const proto::ProtoObject* emptied = protoClojure::mapDissoc(ctx, markers, a, kw(":a"));
    EXPECT_EQ(protoClojure::mapCount(ctx, built), 0u);
    EXPECT_EQ(protoClojure::mapCount(ctx, emptied), 0u);
    EXPECT_TRUE(mapsEqual(ctx, nullptr, built));
    EXPECT_TRUE(mapsEqual(ctx, built, emptied));
    EXPECT_FALSE(mapsEqual(ctx, nullptr, a));

    // Numeric keys of different types are different keys.
    EXPECT_FALSE(mapsEqual(ctx, assoc(nullptr, num(1), kw(":a")),
                           assoc(nullptr, dbl(1.0), kw(":a"))));
}

TEST_F(MapOpsFixture, EqualityComparesValuesThroughTheCallback) {
    const proto::ProtoObject* inner1 = assoc(assoc(nullptr, kw(":x"), num(1)), kw(":y"), num(2));
    const proto::ProtoObject* inner2 = assoc(assoc(nullptr, kw(":y"), num(2)), kw(":x"), num(1));
    const proto::ProtoObject* inner3 = assoc(assoc(nullptr, kw(":y"), num(3)), kw(":x"), num(1));
    EXPECT_TRUE(mapsEqual(ctx, assoc(nullptr, kw(":m"), inner1), assoc(nullptr, kw(":m"), inner2)));
    EXPECT_FALSE(mapsEqual(ctx, assoc(nullptr, kw(":m"), inner1), assoc(nullptr, kw(":m"), inner3)));

    auto never = [](proto::ProtoContext*, void*, const proto::ProtoObject*,
                    const proto::ProtoObject*) { return false; };
    EXPECT_FALSE(protoClojure::mapEquals(ctx, inner1, inner2, nullptr, never));
    EXPECT_TRUE(protoClojure::mapEquals(ctx, inner1, inner1, nullptr, never));
}

TEST_F(MapOpsFixture, MapsWithCollectionKeysCompareByValue) {
    const proto::ProtoObject* k1 = assoc(assoc(nullptr, kw(":a"), num(1)), kw(":b"), num(2));
    const proto::ProtoObject* k2 = assoc(assoc(nullptr, kw(":b"), num(2)), kw(":a"), num(1));
    EXPECT_TRUE(mapsEqual(ctx, assoc(nullptr, k1, num(10)), assoc(nullptr, k2, num(10))));
    EXPECT_FALSE(mapsEqual(ctx, assoc(nullptr, k1, num(10)), assoc(nullptr, k2, num(11))));
    EXPECT_TRUE(mapsEqual(ctx, assoc(nullptr, vec({num(1)}), num(1)),
                          assoc(nullptr, list({num(1)}), num(1))));
}

} // namespace
