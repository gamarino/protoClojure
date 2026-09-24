/*
 * MapOps — the single implementation of protoClojure's runtime map.
 *
 * Representation. A map IS an immutable protoCore `ProtoMap`, with no wrapper
 * object and no mutable state, driven through protoCore's shared
 * hashed-collection helper (`hashedPut` / `hashedGet` / `hashedRemove` /
 * `hashedForEach`, protoCore.h) with the one `KeySemantics` returned by
 * clojureKeySemantics(). The helper decides where an entry lives:
 *   - a key whose Clojure map-key equality IS pointer identity (a keyword, a
 *     symbol, nil, a boolean, an atom, a function, an actor, ...) is stored
 *     under itself, one entry per slot;
 *   - every other key (a string, a double, a big integer, a list, a vector, a
 *     map) is stored under a SmallInteger slot holding the low 54 bits of
 *     keyHash, with the entries of that slot in one flat bucket list
 *     [k0 v0 k1 v1 ...] that keyEquals disambiguates.
 * Both kinds hold the original key object, which is what walks return.
 *
 * Costs:
 *   - get / contains? / assoc / dissoc: one keyHash plus O(log n), with the
 *     hash O(1) for a keyword or an integer and O(size) for a string, a big
 *     integer or a collection key;
 *   - assoc of a key already present keeps the stored key object and replaces
 *     the value, and returns the same map when the new value is the identical
 *     object;
 *   - count is O(n): a genuine 54-bit hash collision puts two entries in one
 *     slot, so the slot count of the ProtoMap would undercount (deviation
 *     D24). Emptiness alone is O(1), because a slot always holds at least one
 *     entry — use mapIsEmpty for it.
 *   - walks (printing, keys, vals, =) visit the entries in the ProtoMap's
 *     ascending slot-word order. Identity slots are ordered by an address and
 *     hashed slots by a hash, so that order is unspecified and can differ
 *     between runs of the same program; keys and vals of one map value still
 *     line up. Clojure guarantees no map order and neither does this.
 *
 * Key semantics (keyEquals / keyHash). Two keys name one entry exactly when
 * keyEquals holds, and keyHash agrees with it:
 *   - keywords and symbols (interned Named values, Named.h), nil, booleans,
 *     atoms, functions, futures, promises and actors: pointer identity;
 *   - strings: by content, whatever form protoCore stores them in (inline,
 *     rope or symbol) — so a string built at run time finds a literal key;
 *   - doubles: by their exact 64-bit pattern. -0.0 and 0.0 are different
 *     keys; a NaN finds only a NaN with the same bit pattern;
 *   - big integers: by value (protoCore never stores a value of the
 *     SmallInteger range as a LargeInteger, so integers need no special case);
 *   - lists and vectors: by their elements, element-wise, so a list and a
 *     vector with equal elements are one key;
 *   - maps: by their entries, each key by keyEquals and each value by
 *     keyEquals too.
 * Numbers of different types are different keys, as in JVM Clojure, even
 * though `=` holds between them (deviation D15): 1 and 1.0, 0 and -0.0, and a
 * big integer and an equal double. The rules apply at every depth of a
 * collection key.
 *
 * Memory. Nothing is interned and nothing is retained: a key is stored as the
 * object the caller passed, and dies with the last map that holds it. That is
 * the point of this layer (it replaced an interned canonical-key scheme whose
 * keys were never freed, deviation D23).
 *
 * GC rooting (engineering principles P1/P2): builders keep intermediates in
 * automatic locals of a child ProtoContext. Returned maps are UNROOTED; the
 * caller roots them before its next allocation. Maps, keys and values passed
 * in must be rooted by the caller.
 */
#pragma once

namespace proto {
class ProtoContext;
class ProtoObject;
class ProtoList;
class ProtoMap;
struct KeySemantics;
}

namespace protoClojure {

// True when `v` is a map: a protoCore ProtoMap in its AVL or its small form
// (pointer tag 27 of protoCore's headers/proto_internal.h, shared by both).
// The runtime never hands a ProtoMap to user code for anything else.
inline bool isMap(const proto::ProtoObject* v) {
    constexpr unsigned long kTagMask = 0x3F;
    constexpr unsigned long kTagMap  = 27;
    const unsigned long tag = reinterpret_cast<unsigned long>(v) & kTagMask;
    return v != nullptr && tag == kTagMap;
}

// The Clojure key semantics handed to protoCore's hashed-collection helper.
// Exported so tests can assert which slot kind a key lands in: the two kinds
// are observationally identical through the language, so only a white-box
// test can catch a misclassification.
const proto::KeySemantics& clojureKeySemantics();

// True when `key`'s map-key equality is pointer identity (the header's
// first group). Never true for a SmallInteger, which protoCore's helper
// always routes through the hashed path.
bool keyIsIdentity(proto::ProtoContext* ctx, const proto::ProtoObject* key);

// The hash of `key` under the semantics above: equal keys hash equally.
// Total, so it is defined for identity keys too (a collection key hashes its
// elements whatever they are).
unsigned long keyHash(proto::ProtoContext* ctx, const proto::ProtoObject* key);

// True when `a` and `b` name one map entry (the semantics above).
bool keyEquals(proto::ProtoContext* ctx, const proto::ProtoObject* a,
               const proto::ProtoObject* b);

// Return `base` (a map, or nullptr for the empty map) with `n` alternating
// keys and values from `kv` associated in order; a repeated key keeps its
// first key object and its last value. `n` must be even.
const proto::ProtoObject* mapAssocPairs(proto::ProtoContext* ctx,
                                        const proto::ProtoObject* base,
                                        const proto::ProtoObject* const* kv,
                                        unsigned long n);

// Same, taking the `n` keys and values from `items[from .. from+n)`.
const proto::ProtoObject* mapAssocPairs(proto::ProtoContext* ctx,
                                        const proto::ProtoObject* base,
                                        const proto::ProtoList* items,
                                        unsigned long from,
                                        unsigned long n);

// Return `m` without `key`. Returns `m` itself when the key is absent.
const proto::ProtoObject* mapDissoc(proto::ProtoContext* ctx,
                                    const proto::ProtoObject* m,
                                    const proto::ProtoObject* key);

// Look `key` up in map `m` (nullptr is the empty map). Sets `*found` and
// returns the value, or PROTO_NONE when absent.
const proto::ProtoObject* mapGet(proto::ProtoContext* ctx,
                                 const proto::ProtoObject* m,
                                 const proto::ProtoObject* key,
                                 bool* found);

// Number of entries in map `m` (nullptr is the empty map). O(n): see the
// note on count in the header comment. Use mapIsEmpty when only emptiness
// matters.
unsigned long mapCount(proto::ProtoContext* ctx, const proto::ProtoObject* m);

// True when `m` holds no entry. O(1).
bool mapIsEmpty(proto::ProtoContext* ctx, const proto::ProtoObject* m);

// Call `fn(ctx, self, key, value)` for every entry of map `m`, with the
// original key, in unspecified order. `m` is pinned for the duration of the
// walk. protoCore's ProtoMap::processElements holds no GC critical section,
// so `fn` may allocate and run arbitrary code.
using MapEntryFn = void (*)(proto::ProtoContext* ctx, void* self,
                            const proto::ProtoObject* key,
                            const proto::ProtoObject* value);
void mapForEach(proto::ProtoContext* ctx, const proto::ProtoObject* m,
                void* self, MapEntryFn fn);

// Value equality of two maps, as used by `=`: true when both hold the same
// number of entries and every key of `a` names an entry of `b` whose value
// satisfies `valueEq(ctx, self, valueInA, valueInB)`. nullptr is the empty
// map. Walks `a` and probes `b`, O(n log n); allocates nothing itself.
using MapValueEqFn = bool (*)(proto::ProtoContext* ctx, void* self,
                              const proto::ProtoObject* a,
                              const proto::ProtoObject* b);
bool mapEquals(proto::ProtoContext* ctx,
               const proto::ProtoObject* a, const proto::ProtoObject* b,
               void* self, MapValueEqFn valueEq);

} // namespace protoClojure
