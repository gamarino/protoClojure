#include "MapOps.h"
#include "StackGuard.h"
#include "VectorOps.h"

#include "protoCore.h"

#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace protoClojure {

namespace {

// Pointer tags of protoCore's headers/proto_internal.h.
constexpr unsigned long kTagMask         = 0x3F;
constexpr unsigned long kTagObject       = 0;
constexpr unsigned long kTagEmbedded     = 1;
constexpr unsigned long kTagList         = 2;
constexpr unsigned long kTagLargeInteger = 14;
constexpr unsigned long kTagDouble       = 15;
constexpr unsigned long kTagListSmall    = 25;

// One salt per key class, so keys of different classes never share a hash by
// construction: a string, a one-element vector and a big integer that happen
// to reduce to the same number still land in different slots.
constexpr unsigned long kSaltIdentity = 0x9E3779B97F4A7C15UL;
constexpr unsigned long kSaltInteger  = 0xC2B2AE3D27D4EB4FUL;
constexpr unsigned long kSaltDouble   = 0x165667B19E3779F9UL;
constexpr unsigned long kSaltBigInt   = 0x27D4EB2F165667C5UL;
constexpr unsigned long kSaltString   = 0xD6E8FEB86659FD93UL;
constexpr unsigned long kSaltSequence = 0xA24BAED4963EE407UL;
constexpr unsigned long kSaltMap      = 0x9FB21C651E98DF25UL;

// Automatic-local slots of a MapBuilder scope.
constexpr unsigned int kSlotMap   = 0;
constexpr unsigned int kSlotCount = 1;

inline unsigned long tagOf(const proto::ProtoObject* v) {
    return reinterpret_cast<unsigned long>(v) & kTagMask;
}

// The tag has already been checked by isMap / isSequential, so the cast is
// the whole conversion: ProtoObject::asMap would first probe a tag-0 object
// for a `__data__` wrapper attribute, which this runtime never sets.
inline const proto::ProtoMap* asMapFast(const proto::ProtoObject* m) {
    return reinterpret_cast<const proto::ProtoMap*>(m);
}

inline bool isListTag(const proto::ProtoObject* v) {
    const unsigned long tag = tagOf(v);
    return tag == kTagList || tag == kTagListSmall;
}

inline bool isSequential(const proto::ProtoObject* v) {
    return v != nullptr && (isVector(v) || isListTag(v));
}

// A list or a vector read by index. A vector's elements are a ProtoList
// too (VectorOps.h), so one view covers both.
struct SequenceView {
    const proto::ProtoList* list = nullptr;

    static SequenceView of(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
        SequenceView s;
        s.list = isVector(v) ? vectorItems(ctx, v) : v->asList(ctx);
        return s;
    }
    unsigned long size(proto::ProtoContext* ctx) const { return list->getSize(ctx); }
    const proto::ProtoObject* at(proto::ProtoContext* ctx, unsigned long i) const {
        return list->getAt(ctx, static_cast<int>(i));
    }
};

// One round of the SplitMix64 finaliser: cheap, and it spreads the low bits,
// which is what the helper's 54-bit slot key keeps.
inline unsigned long mix(unsigned long h) {
    std::uint64_t x = static_cast<std::uint64_t>(h);
    x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27; x *= 0x94D049BB133111EBULL;
    x ^= x >> 31;
    return static_cast<unsigned long>(x);
}

inline unsigned long combine(unsigned long acc, unsigned long value) {
    return mix(acc ^ (value + 0x9E3779B97F4A7C15UL + (acc << 6) + (acc >> 2)));
}

std::uint64_t doubleBits(proto::ProtoContext* ctx, const proto::ProtoObject* key) {
    const double value = key->asDouble(ctx);
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

unsigned long sequenceHash(proto::ProtoContext* ctx, const proto::ProtoObject* seq) {
    const SequenceView view = SequenceView::of(ctx, seq);
    const unsigned long n = view.size(ctx);
    unsigned long h = combine(kSaltSequence, n);
    for (unsigned long i = 0; i < n; ++i) h = combine(h, keyHash(ctx, view.at(ctx, i)));
    return h;
}

struct MapHashAccumulator {
    unsigned long sum   = 0;
    unsigned long count = 0;
};

void accumulateEntryHash(proto::ProtoContext* ctx, void* self,
                         const proto::ProtoObject* key,
                         const proto::ProtoObject* value) {
    auto* acc = static_cast<MapHashAccumulator*>(self);
    // Added, not combined: the sum must not depend on the walk order, which
    // is unspecified and differs between two equal maps built differently.
    acc->sum += combine(keyHash(ctx, key), keyHash(ctx, value));
    ++acc->count;
}

unsigned long mapHash(proto::ProtoContext* ctx, const proto::ProtoObject* m) {
    MapHashAccumulator acc;
    proto::hashedForEach(ctx, asMapFast(m), &acc, &accumulateEntryHash);
    return combine(combine(kSaltMap, acc.count), acc.sum);
}

bool sequenceEquals(proto::ProtoContext* ctx, const proto::ProtoObject* a,
                    const proto::ProtoObject* b) {
    const SequenceView va = SequenceView::of(ctx, a);
    const SequenceView vb = SequenceView::of(ctx, b);
    const unsigned long n = va.size(ctx);
    if (n != vb.size(ctx)) return false;
    for (unsigned long i = 0; i < n; ++i)
        if (!keyEquals(ctx, va.at(ctx, i), vb.at(ctx, i))) return false;
    return true;
}

struct MapEqualsState {
    const proto::ProtoMap* other;
    bool                   equal;
};

void compareEntryAsKey(proto::ProtoContext* ctx, void* self,
                       const proto::ProtoObject* key,
                       const proto::ProtoObject* value) {
    auto* state = static_cast<MapEqualsState*>(self);
    if (!state->equal) return;  // hashedForEach cannot stop early
    const proto::ProtoObject* mine =
        proto::hashedGet(ctx, state->other, clojureKeySemantics(), key);
    state->equal = mine != nullptr && keyEquals(ctx, value, mine);
}

// Two maps are one key when they hold the same entries, each value compared
// as a key would be (so {1 :a} and {1.0 :a} are different keys, like 1 and
// 1.0 themselves).
bool mapKeyEquals(proto::ProtoContext* ctx, const proto::ProtoObject* a,
                  const proto::ProtoObject* b) {
    if (mapCount(ctx, a) != mapCount(ctx, b)) return false;
    MapEqualsState state{asMapFast(b), true};
    proto::hashedForEach(ctx, asMapFast(a), &state, &compareEntryAsKey);
    return state.equal;
}

struct CountAccumulator {
    unsigned long n = 0;
};

void countEntry(proto::ProtoContext*, void* self, const proto::ProtoObject*,
                const proto::ProtoObject*) {
    ++static_cast<CountAccumulator*>(self)->n;
}

// A working copy of one map, rooted in the slots of a child context.
// Construct it on the stack only (contexts nest LIFO).
class MapBuilder {
public:
    MapBuilder(proto::ProtoContext* parent, const proto::ProtoObject* base)
        : scope_(parent->space, parent) {
        scope_.resizeAutomaticLocals(kSlotCount);
        scope_.setAutomaticLocal(kSlotMap,
            (base && base != PROTO_NONE) ? base : scope_.newMap()->asObject(&scope_));
    }

    proto::ProtoContext* ctx() { return &scope_; }

    // A key already present keeps its stored key object and takes the new
    // value; an identical value changes nothing.
    void assoc(const proto::ProtoObject* key, const proto::ProtoObject* value) {
        if (!key) key = PROTO_NONE;
        // A nullptr value means "remove" to the helper, and nil is a value
        // here: a key mapped to nil is an entry.
        if (!value) value = PROTO_NONE;
        scope_.setAutomaticLocal(kSlotMap,
            proto::hashedPut(&scope_, slotMap(), clojureKeySemantics(), key, value)
                ->asObject(&scope_));
    }

    // Returns false, changing nothing, when `key` is absent.
    bool dissoc(const proto::ProtoObject* key) {
        if (!key) key = PROTO_NONE;
        const proto::ProtoMap* before = slotMap();
        // Probe first: ProtoMap::removeAt of an absent key is free to return
        // a new, equal version, and dissoc of an absent key must return the
        // receiver itself.
        if (!proto::hashedGet(&scope_, before, clojureKeySemantics(), key)) return false;
        scope_.setAutomaticLocal(kSlotMap,
            proto::hashedRemove(&scope_, before, clojureKeySemantics(), key)
                ->asObject(&scope_));
        return true;
    }

    // The map built so far. UNROOTED once this builder goes out of scope.
    const proto::ProtoObject* result() { return scope_.getAutomaticLocal(kSlotMap); }

private:
    const proto::ProtoMap* slotMap() {
        return asMapFast(scope_.getAutomaticLocal(kSlotMap));
    }

    proto::ProtoContext scope_;
};

void requireEven(unsigned long n) {
    if (n % 2 != 0)
        throw std::runtime_error("map: a key has no value");
}

} // namespace

bool keyIsIdentity(proto::ProtoContext* /*ctx*/, const proto::ProtoObject* key) {
    if (!key) return true;
    // A string is matched by content whatever protoCore stores it in: a
    // pointer tag (6, 22) or an inline string in an embedded word.
    if (proto::ProtoObject::isStringTagFast(key)) return false;
    switch (tagOf(key)) {
        case kTagEmbedded:
            // nil, booleans and characters are unique words, so identity is
            // their equality. A SmallInteger never reaches this answer:
            // protoCore's helper always routes one through the hashed path.
            return !proto::isSmallInt(key);
        case kTagDouble:
        case kTagLargeInteger:
        case kTagList:
        case kTagListSmall:
            return false;
        case kTagObject:
            // Keywords and symbols (interned Named values), atoms, futures,
            // promises, actors: equal only to themselves.
            return true;
        default:
            // A map and a vector are matched by value; anything else
            // protoCore may hand out (a method, a thread, a byte buffer) is
            // equal only to itself.
            return !isMap(key) && !isVector(key);
    }
}

unsigned long keyHash(proto::ProtoContext* ctx, const proto::ProtoObject* key) {
    if (!key) key = PROTO_NONE;
    if (proto::ProtoObject::isStringTagFast(key)) {
        // The content hash, equal for the inline, rope and symbol forms of
        // one text (protoCore's getProtoStringHash).
        return combine(kSaltString,
                       reinterpret_cast<const proto::ProtoString*>(key)->getHash(ctx));
    }
    switch (tagOf(key)) {
        case kTagEmbedded:
            return combine(proto::isSmallInt(key) ? kSaltInteger : kSaltIdentity,
                           reinterpret_cast<unsigned long>(key));
        case kTagDouble:
            // The exact bit pattern, because that is what keyEquals compares:
            // 0.0 and -0.0, and two NaNs with different payloads, are
            // different keys and hash differently.
            return combine(kSaltDouble, static_cast<unsigned long>(doubleBits(ctx, key)));
        case kTagLargeInteger:
            // protoCore hashes every digit of the canonical representation,
            // so equal values computed differently hash equally.
            return combine(kSaltBigInt, key->getHash(ctx));
        case kTagList:
        case kTagListSmall:
            checkNativeStack();
            return sequenceHash(ctx, key);
        default:
            if (isVector(key)) {
                checkNativeStack();
                return sequenceHash(ctx, key);
            }
            if (isMap(key)) {
                checkNativeStack();
                return mapHash(ctx, key);
            }
            return combine(kSaltIdentity, reinterpret_cast<unsigned long>(key));
    }
}

bool keyEquals(proto::ProtoContext* ctx, const proto::ProtoObject* a,
               const proto::ProtoObject* b) {
    if (!a) a = PROTO_NONE;
    if (!b) b = PROTO_NONE;
    if (a == b) return true;
    // Recurses once per level of nesting (StackGuard.h).
    checkNativeStack();

    const bool aString = proto::ProtoObject::isStringTagFast(a);
    const bool bString = proto::ProtoObject::isStringTagFast(b);
    if (aString || bString) return aString && bString && a->partialCompare(ctx, b) == 0;

    // A list and a vector with equal elements are one key.
    const bool aSeq = isSequential(a);
    const bool bSeq = isSequential(b);
    if (aSeq || bSeq) return aSeq && bSeq && sequenceEquals(ctx, a, b);

    const bool aMap = isMap(a);
    const bool bMap = isMap(b);
    if (aMap || bMap) return aMap && bMap && mapKeyEquals(ctx, a, b);

    if (tagOf(a) != tagOf(b)) return false;
    switch (tagOf(a)) {
        case kTagDouble:
            return doubleBits(ctx, a) == doubleBits(ctx, b);
        case kTagLargeInteger:
            return a->partialCompare(ctx, b) == 0;
        default:
            // Identity keys, and SmallIntegers, which are unique words: the
            // pointer test above already decided them.
            return false;
    }
}

const proto::KeySemantics& clojureKeySemantics() {
    static const proto::KeySemantics semantics{&keyIsIdentity, &keyHash, &keyEquals};
    return semantics;
}

const proto::ProtoObject* mapAssocPairs(proto::ProtoContext* ctx,
                                        const proto::ProtoObject* base,
                                        const proto::ProtoObject* const* kv,
                                        unsigned long n) {
    requireEven(n);
    MapBuilder b(ctx, base);
    for (unsigned long i = 0; i < n; i += 2) b.assoc(kv[i], kv[i + 1]);
    return b.result();
}

const proto::ProtoObject* mapAssocPairs(proto::ProtoContext* ctx,
                                        const proto::ProtoObject* base,
                                        const proto::ProtoList* items,
                                        unsigned long from,
                                        unsigned long n) {
    requireEven(n);
    MapBuilder b(ctx, base);
    for (unsigned long i = from; i < from + n; i += 2) {
        b.assoc(items->getAt(b.ctx(), static_cast<int>(i)),
                items->getAt(b.ctx(), static_cast<int>(i + 1)));
    }
    return b.result();
}

const proto::ProtoObject* mapDissoc(proto::ProtoContext* ctx,
                                    const proto::ProtoObject* m,
                                    const proto::ProtoObject* key) {
    if (mapIsEmpty(ctx, m)) return m;
    MapBuilder b(ctx, m);
    if (!b.dissoc(key)) return m;
    return b.result();
}

const proto::ProtoObject* mapGet(proto::ProtoContext* ctx,
                                 const proto::ProtoObject* m,
                                 const proto::ProtoObject* key,
                                 bool* found) {
    *found = false;
    if (!m || m == PROTO_NONE) return PROTO_NONE;
    const proto::ProtoObject* value =
        proto::hashedGet(ctx, asMapFast(m), clojureKeySemantics(), key ? key : PROTO_NONE);
    if (!value) return PROTO_NONE;
    *found = true;
    return value;
}

unsigned long mapCount(proto::ProtoContext* ctx, const proto::ProtoObject* m) {
    if (!m || m == PROTO_NONE) return 0;
    // The number of ENTRIES, not of slots: a 54-bit hash collision puts
    // several entries in one slot, so ProtoMap::getSize would undercount.
    CountAccumulator acc;
    proto::hashedForEach(ctx, asMapFast(m), &acc, &countEntry);
    return acc.n;
}

bool mapIsEmpty(proto::ProtoContext* ctx, const proto::ProtoObject* m) {
    // Only emptiness can be read off the slot count: a slot always holds at
    // least one entry.
    return !m || m == PROTO_NONE || asMapFast(m)->getSize(ctx) == 0;
}

void mapForEach(proto::ProtoContext* ctx, const proto::ProtoObject* m,
                void* self, MapEntryFn fn) {
    if (!m || m == PROTO_NONE) return;
    // Pin the map: a callback may replace the only other reference to it.
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(1);
    scope.setAutomaticLocal(0, m);
    struct Adapter {
        void*      self;
        MapEntryFn fn;
    } adapter{self, fn};
    proto::hashedForEach(&scope, asMapFast(m), &adapter,
        [](proto::ProtoContext* c, void* a, const proto::ProtoObject* key,
           const proto::ProtoObject* value) {
            const auto* ad = static_cast<Adapter*>(a);
            ad->fn(c, ad->self, key, value);
        });
}

bool mapEquals(proto::ProtoContext* ctx,
               const proto::ProtoObject* a, const proto::ProtoObject* b,
               void* self, MapValueEqFn valueEq) {
    if (a == b) return true;
    const unsigned long n = mapCount(ctx, a);
    if (n != mapCount(ctx, b)) return false;
    if (n == 0) return true;

    // Equal counts plus every key of `a` present in `b` means the key sets
    // are equal.
    struct Walk {
        const proto::ProtoMap* other;
        void*                  self;
        MapValueEqFn           valueEq;
        bool                   equal;
    } walk{asMapFast(b), self, valueEq, true};
    proto::hashedForEach(ctx, asMapFast(a), &walk,
        [](proto::ProtoContext* c, void* p, const proto::ProtoObject* key,
           const proto::ProtoObject* value) {
            auto* w = static_cast<Walk*>(p);
            if (!w->equal) return;  // hashedForEach cannot stop early
            const proto::ProtoObject* mine =
                proto::hashedGet(c, w->other, clojureKeySemantics(), key);
            if (!mine) { w->equal = false; return; }
            w->equal = w->valueEq(c, w->self, value, mine);
        });
    return walk.equal;
}

} // namespace protoClojure
