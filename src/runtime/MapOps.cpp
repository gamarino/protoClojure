#include "CallerSlots.h"
#include "MapOps.h"
#include "Named.h"
#include "SeqTypes.h"
#include "SetOps.h"
#include "StackGuard.h"
#include "VectorOps.h"

#include "protoCore.h"

#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace protoClojure {

namespace {

// One salt per key class, so keys of different classes never share a hash by
// construction: a string, a one-element vector and a big integer that happen
// to reduce to the same number still land in different slots.
constexpr proto::proto_ulong kSaltIdentity = PROTO_UL(0x9E3779B97F4A7C15);
constexpr proto::proto_ulong kSaltInteger  = PROTO_UL(0xC2B2AE3D27D4EB4F);
constexpr proto::proto_ulong kSaltDouble   = PROTO_UL(0x165667B19E3779F9);
constexpr proto::proto_ulong kSaltBigInt   = PROTO_UL(0x27D4EB2F165667C5);
constexpr proto::proto_ulong kSaltString   = PROTO_UL(0xD6E8FEB86659FD93);
constexpr proto::proto_ulong kSaltSequence = PROTO_UL(0xA24BAED4963EE407);
constexpr proto::proto_ulong kSaltMap      = PROTO_UL(0x9FB21C651E98DF25);

// CallerSlots slots of a MapBuilder.
constexpr unsigned int kSlotMap   = 0;
constexpr unsigned int kSlotCount = 1;

// The kind has already been checked by isMap / isSequential, so the cast is
// the whole conversion: ProtoObject::asMap would first probe a tag-0 object
// for a `__data__` wrapper attribute, which this runtime never sets.
inline const proto::ProtoMap* asMapFast(const proto::ProtoObject* m) {
    return reinterpret_cast<const proto::ProtoMap*>(m);
}

inline bool isListTag(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
    return isProtoList(ctx, v);
}

// A vector, a Clojure list (SeqTypes.h), or the ProtoList a seq object was
// read into (keyHash, keyEquals): what is matched element by element.
inline bool isSequential(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
    return v != nullptr && (isVector(ctx, v) || isListTag(ctx, v) || isListValue(ctx, v));
}

// The elements of a sequential value in order: a vector's or a ProtoList's
// by index, a list's along its cells. No allocation; the value must stay
// reachable while it is read.
struct SequenceView {
    const proto::ProtoList* list = nullptr;  // a vector's elements, or a ProtoList
    proto::proto_ulong n = 0, i = 0;

    static SequenceView of(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
        SequenceView s;
        if (isVector(ctx, v))          s.list = vectorItems(ctx, v);
        else if (isListTag(ctx, v))    s.list = v->asList(ctx);
        else                      s.cursor = ListCursor(v);
        s.n = s.list ? s.list->getSize(ctx) : static_cast<proto::proto_ulong>(listCount(ctx, v));
        return s;
    }
    proto::proto_ulong size() const { return n; }
    const proto::ProtoObject* next(proto::ProtoContext* ctx) {
        ++i;
        return list ? list->getAt(ctx, static_cast<int>(i - 1)) : cursor.next();
    }

    ListCursor cursor{emptyListValue()};  // a list's elements
};

// One round of the SplitMix64 finaliser: cheap, and it spreads the low bits,
// which is what the helper's 54-bit slot key keeps.
inline proto::proto_ulong mix(proto::proto_ulong h) {
    std::uint64_t x = static_cast<std::uint64_t>(h);
    x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27; x *= 0x94D049BB133111EBULL;
    x ^= x >> 31;
    return static_cast<proto::proto_ulong>(x);
}

inline proto::proto_ulong combine(proto::proto_ulong acc, proto::proto_ulong value) {
    return mix(acc ^ (value + PROTO_UL(0x9E3779B97F4A7C15) + (acc << 6) + (acc >> 2)));
}

std::uint64_t doubleBits(proto::ProtoContext* ctx, const proto::ProtoObject* key) {
    const double value = key->asDouble(ctx);
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

proto::proto_ulong sequenceHash(proto::ProtoContext* ctx, const proto::ProtoObject* seq) {
    SequenceView view = SequenceView::of(ctx, seq);
    const proto::proto_ulong n = view.size();
    proto::proto_ulong h = combine(kSaltSequence, n);
    for (proto::proto_ulong i = 0; i < n; ++i) h = combine(h, keyHash(ctx, view.next(ctx)));
    return h;
}

struct MapHashAccumulator {
    proto::proto_ulong sum   = 0;
    proto::proto_ulong count = 0;
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

proto::proto_ulong mapHash(proto::ProtoContext* ctx, const proto::ProtoObject* m) {
    MapHashAccumulator acc;
    proto::hashedForEach(ctx, asMapFast(m), &acc, &accumulateEntryHash);
    return combine(combine(kSaltMap, acc.count), acc.sum);
}

bool sequenceEquals(proto::ProtoContext* ctx, const proto::ProtoObject* a,
                    const proto::ProtoObject* b) {
    SequenceView va = SequenceView::of(ctx, a);
    SequenceView vb = SequenceView::of(ctx, b);
    const proto::proto_ulong n = va.size();
    if (n != vb.size()) return false;
    for (proto::proto_ulong i = 0; i < n; ++i)
        if (!keyEquals(ctx, va.next(ctx), vb.next(ctx))) return false;
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
    proto::proto_ulong n = 0;
};

void countEntry(proto::ProtoContext*, void* self, const proto::ProtoObject*,
                const proto::ProtoObject*) {
    ++static_cast<CountAccumulator*>(self)->n;
}

// A working copy of one map, in a CallerSlots slot of the caller's context
// (no context of its own, CallerSlots.h). Construct it on the stack only
// (the slots are a stack).
class MapBuilder {
public:
    MapBuilder(proto::ProtoContext* ctx, const proto::ProtoObject* base)
        : ctx_(ctx), slots_(ctx, kSlotCount) {
        slots_.set(kSlotMap,
            (base && base != PROTO_NONE) ? base : ctx_->newMap()->asObject(ctx_));
    }

    proto::ProtoContext* ctx() { return ctx_; }

    // A key already present keeps its stored key object and takes the new
    // value; an identical value changes nothing.
    void assoc(const proto::ProtoObject* key, const proto::ProtoObject* value) {
        if (!key) key = PROTO_NONE;
        // A nullptr value means "remove" to the helper, and nil is a value
        // here: a key mapped to nil is an entry.
        if (!value) value = PROTO_NONE;
        slots_.set(kSlotMap,
            proto::hashedPut(ctx_, slotMap(), clojureKeySemantics(), key, value)
                ->asObject(ctx_));
    }

    // Returns false, changing nothing, when `key` is absent.
    bool dissoc(const proto::ProtoObject* key) {
        if (!key) key = PROTO_NONE;
        const proto::ProtoMap* before = slotMap();
        // Probe first: ProtoMap::removeAt of an absent key is free to return
        // a new, equal version, and dissoc of an absent key must return the
        // receiver itself.
        if (!proto::hashedGet(ctx_, before, clojureKeySemantics(), key)) return false;
        slots_.set(kSlotMap,
            proto::hashedRemove(ctx_, before, clojureKeySemantics(), key)
                ->asObject(ctx_));
        return true;
    }

    // The map built so far: a young cell of the caller's context (or held
    // by the caller's arguments), unrooted once this builder is gone.
    const proto::ProtoObject* result() { return slots_.get(kSlotMap); }

private:
    const proto::ProtoMap* slotMap() {
        return asMapFast(slots_.get(kSlotMap));
    }

    proto::ProtoContext* ctx_;
    CallerSlots          slots_;
};

void requireEven(proto::proto_ulong n) {
    if (n % 2 != 0)
        throw std::runtime_error("map: a key has no value");
}

} // namespace

namespace {

// The key classes of Clojure's key semantics, decided together so that a
// key pays for its kind once. `v` is a cell (not a SmallInteger, not an
// embedded word) and not a string.
struct KeyClass {
    bool sequential = false;  // a vector, a ProtoList or a Clojure list (SeqTypes.h)
    bool seqNode = false;     // a lazy seq, a cons or a chunk: sequential once realised
    bool map = false;
    bool set = false;
    bool dbl = false;
    bool bigInt = false;
};

KeyClass keyClassOf(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
    const proto::ProtoSpace* sp = ctx->space;
    KeyClass c;
    const proto::ProtoObject* p = v->getPrototype(ctx);
    if (v->getFirstParent(ctx) != PROTO_NONE) {
        // An object cell with a parent: a set, a seq node, or an identity key.
        c.set = g_setPrototype && p == g_setPrototype;
        c.seqNode = g_seqProtos.lazy &&
                    (p == g_seqProtos.lazy || p == g_seqProtos.cons || p == g_seqProtos.chunk);
        return c;
    }
    // Kinds with a ProtoSpace prototype of their own.
    c.sequential = p == sp->sparseListPrototype || p == sp->listPrototype ||
                   p == sp->getLinkedListPrototype();
    c.map = p == sp->mapPrototype;
    if (c.sequential || c.map) return c;
    // Doubles and LargeIntegers, whose prototypes may alias objectPrototype.
    c.dbl = v->isDouble(ctx);
    c.bigInt = !c.dbl && v->isInteger(ctx);
    return c;
}

} // namespace

bool keyIsIdentity(proto::ProtoContext* ctx, const proto::ProtoObject* key) {
    if (!key) return true;
    // A string is matched by content whatever protoCore stores it in: a heap
    // string, a symbol or an inline string.
    if (proto::ProtoObject::isStringTagFast(key)) return false;
    // nil, booleans and characters are unique words, so identity is their
    // equality. A SmallInteger never reaches this answer: protoCore's helper
    // always routes one through the hashed path.
    if (proto::isSmallInt(key)) return false;
    if (!proto::ProtoObject::isCellPointer(key)) return true;
    // Keywords and symbols (interned Named values) are found in the
    // named-value cache (Named.h) without reading a prototype.
    if (isKnownNamed(key)) return true;
    // Atoms, futures, promises, actors, methods, threads and byte buffers are
    // equal only to themselves. Numbers that are not unique words, maps,
    // sets, and every sequential value (vectors, ProtoLists, Clojure lists,
    // lazy seqs, conses, chunks) are matched by value.
    const KeyClass c = keyClassOf(ctx, key);
    return !(c.sequential || c.seqNode || c.map || c.set || c.dbl || c.bigInt);
}

proto::proto_ulong keyHash(proto::ProtoContext* ctx, const proto::ProtoObject* key) {
    if (!key) key = PROTO_NONE;
    if (proto::ProtoObject::isStringTagFast(key)) {
        // The content hash, equal for the inline, rope and symbol forms of
        // one text (protoCore's getProtoStringHash).
        return combine(kSaltString,
                       reinterpret_cast<const proto::ProtoString*>(key)->getHash(ctx));
    }
    if (proto::isSmallInt(key)) return combine(kSaltInteger, reinterpret_cast<proto::proto_ulong>(key));
    // nil, booleans and characters: unique words, hashed by identity.
    if (!proto::ProtoObject::isCellPointer(key))
        return combine(kSaltIdentity, reinterpret_cast<proto::proto_ulong>(key));
    // Keywords and symbols (Named.h): identity keys, found without a probe.
    if (isKnownNamed(key)) return combine(kSaltIdentity, reinterpret_cast<proto::proto_ulong>(key));
    const KeyClass c = keyClassOf(ctx, key);
    if (c.dbl) {
        // The exact bit pattern, because that is what keyEquals compares:
        // 0.0 and -0.0, and two NaNs with different payloads, are
        // different keys and hash differently.
        return combine(kSaltDouble, static_cast<proto::proto_ulong>(doubleBits(ctx, key)));
    }
    if (c.bigInt) {
        // protoCore hashes every digit of the canonical representation,
        // so equal values computed differently hash equally.
        return combine(kSaltBigInt, key->getHash(ctx));
    }
    if (c.sequential) {
        checkNativeStack();
        return sequenceHash(ctx, key);
    }
    if (c.map) {
        checkNativeStack();
        return mapHash(ctx, key);
    }
    if (c.set) {
        checkNativeStack();
        return setHash(ctx, key);
    }
    if (c.seqNode) {
        // A seq hashes as the list of its elements, so it is the same
        // key as an equal list or vector. Realises it (Clojure's
        // hasheq walks it too).
        checkNativeStack();
        CallerSlots scope(ctx, 1);
        const proto::ProtoList* lst = seqObjectToList(ctx, key);
        if (!lst) lst = ctx->newList();
        scope.set(0, lst->asObject(ctx));
        return sequenceHash(ctx, scope.get(0));
    }
    return combine(kSaltIdentity, reinterpret_cast<proto::proto_ulong>(key));
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

    // Distinct SmallIntegers, and distinct identity words, are never equal.
    if (proto::isSmallInt(a) || proto::isSmallInt(b)) return false;

    const KeyClass ca = keyClassOf(ctx, a);
    const KeyClass cb = keyClassOf(ctx, b);

    // A lazy seq, a cons or a chunk is compared as the list of its elements
    // (SeqTypes.h), each realised in the caller's context; a list is read
    // along its cells.
    if (ca.seqNode || cb.seqNode) {
        CallerSlots scope(ctx, 2);
        const proto::ProtoObject* both[2] = {a, b};
        const bool node[2] = {ca.seqNode, cb.seqNode};
        for (int i = 0; i < 2; ++i) {
            const proto::ProtoObject* v = both[i];
            if (node[i]) {
                const proto::ProtoList* lst = seqObjectToList(ctx, v);
                v = (lst ? lst : ctx->newList())->asObject(ctx);
            }
            scope.set(i, v);
        }
        return keyEquals(ctx, scope.get(0), scope.get(1));
    }

    // A list and a vector with equal elements are one key.
    if (ca.sequential || cb.sequential)
        return ca.sequential && cb.sequential && sequenceEquals(ctx, a, b);

    if (ca.map || cb.map) return ca.map && cb.map && mapKeyEquals(ctx, a, b);

    if (ca.set || cb.set) return ca.set && cb.set && setEquals(ctx, a, b);

    // Doubles by bit pattern, LargeIntegers by value. Identity keys: the
    // pointer test above already decided them.
    if (ca.dbl) return cb.dbl && doubleBits(ctx, a) == doubleBits(ctx, b);
    if (ca.bigInt) return cb.bigInt && a->partialCompare(ctx, b) == 0;
    return false;
}

const proto::KeySemantics& clojureKeySemantics() {
    static const proto::KeySemantics semantics{&keyIsIdentity, &keyHash, &keyEquals};
    return semantics;
}

const proto::ProtoObject* mapAssocPairs(proto::ProtoContext* ctx,
                                        const proto::ProtoObject* base,
                                        const proto::ProtoObject* const* kv,
                                        proto::proto_ulong n) {
    requireEven(n);
    MapBuilder b(ctx, base);
    for (proto::proto_ulong i = 0; i < n; i += 2) b.assoc(kv[i], kv[i + 1]);
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

proto::proto_ulong mapCount(proto::ProtoContext* ctx, const proto::ProtoObject* m) {
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
    CallerSlots scope(ctx, 1);
    scope.set(0, m);
    struct Adapter {
        void*      self;
        MapEntryFn fn;
    } adapter{self, fn};
    proto::hashedForEach(ctx, asMapFast(m), &adapter,
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
    const proto::proto_ulong n = mapCount(ctx, a);
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
