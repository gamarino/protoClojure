/*
 * MapOps — the single implementation of protoClojure's runtime map.
 *
 * Every site that builds, probes or iterates a map goes through this
 * module; no other code reads the map state attribute directly.
 *
 * Layout. A map is a mutable child of the map marker prototype with one
 * attribute (MapLayout::stateKey) holding an immutable 3-element ProtoList:
 *
 *   [0] order    ProtoSparseList  seq -> key
 *   [1] index    ProtoSparseList  key hash -> bucket ProtoList of
 *                                 (key value seq) triples
 *   [2] nextSeq  SmallInt         next sequence number, never decremented
 *
 * `order` is the insertion-order store: a ProtoSparseList walks its keys in
 * ascending order, and a new key receives `nextSeq`, so iteration and
 * printing follow insertion order at every size. `index` holds each key's
 * value and sequence number; a bucket holds more than one triple only on a
 * hash collision. Keys are hashed with valueHash and compared with
 * valuesEqual (src/runtime/Primitives.h), the hash and the equality of `=`.
 *
 * Semantics:
 *   - assoc of a new key appends it at the end;
 *   - assoc of an existing key replaces the value and keeps the key object
 *     and its position (array-map behaviour on the JVM);
 *   - dissoc keeps the relative order of the remaining keys;
 *   - sequence numbers are an implementation detail: map equality
 *     (mapEquals) and map hashing (valueHash, through
 *     mapForEachHashedEntry) ignore them and entry order;
 *   - keys match by value: an equal map, vector or list built separately,
 *     or an equal number of another type (deviation D15), finds, replaces
 *     and removes the entry; a repeated key keeps its first key object.
 *
 * Cost: every operation taking a key hashes it once — O(1) for numbers,
 * strings and keywords, linear in its size for a collection key (hashes are
 * not cached). Beyond that, get is one sparse-list walk plus the bucket
 * scan; assoc of a new key and dissoc update both sparse lists, O(log N);
 * assoc of an existing key updates the index only. Iteration walks `order`
 * and looks each value up in `index`, rehashing each key, O(N log N) for
 * scalar keys. The layout favours lookups and construction over iteration: a first version that also kept (key value) pairs in the
 * order store made construction ~37% and `get` ~10% more expensive in CPU
 * cycles on microbenchmarks.
 *
 * Maps are persistent: every operation returns a new wrapper and never
 * mutates its input, so maps derived from the same parent are independent.
 *
 * GC rooting (engineering principles P1/P2): builders keep every
 * intermediate in automatic locals of a child ProtoContext. Returned maps
 * are UNROOTED; the caller roots them before its next allocation. Keys and
 * values passed in must be rooted by the caller.
 */
#pragma once

namespace proto {
class ProtoContext;
class ProtoObject;
class ProtoString;
class ProtoList;
}

namespace protoClojure {

struct MapLayout {
    const proto::ProtoObject* marker;    // prototype of every map wrapper
    const proto::ProtoString* stateKey;  // attribute holding the map state
    // Key hash function; nullptr means valueHash (Primitives.h). Only unit
    // tests override it (to force hash collisions); a map must always be
    // used with the hash function it was built with.
    unsigned long (*hash)(proto::ProtoContext*, const proto::ProtoObject*) = nullptr;
};

// True when `v` is a map wrapper.
bool isMap(proto::ProtoContext* ctx, const MapLayout& layout,
           const proto::ProtoObject* v);

// Return `base` (a map, or nullptr for the empty map) with `n` alternating
// keys and values from `kv` associated in order; a repeated key keeps its
// last value and its first position. `n` must be even.
const proto::ProtoObject* mapAssocPairs(proto::ProtoContext* ctx,
                                        const MapLayout& layout,
                                        const proto::ProtoObject* base,
                                        const proto::ProtoObject* const* kv,
                                        unsigned long n);

// Same, taking the `n` keys and values from `items[from .. from+n)`.
const proto::ProtoObject* mapAssocPairs(proto::ProtoContext* ctx,
                                        const MapLayout& layout,
                                        const proto::ProtoObject* base,
                                        const proto::ProtoList* items,
                                        unsigned long from,
                                        unsigned long n);

// Return `m` without `key`. Returns `m` itself when the key is absent.
const proto::ProtoObject* mapDissoc(proto::ProtoContext* ctx,
                                    const MapLayout& layout,
                                    const proto::ProtoObject* m,
                                    const proto::ProtoObject* key);

// Look `key` up in map `m`. Sets `*found` and returns the value, or
// PROTO_NONE when absent. Allocates nothing, except when hashing an integer
// beyond the long long range (valueHash).
const proto::ProtoObject* mapGet(proto::ProtoContext* ctx,
                                 const MapLayout& layout,
                                 const proto::ProtoObject* m,
                                 const proto::ProtoObject* key,
                                 bool* found);

// Call `fn(ctx, self, key, value)` for every entry of map `m`, in insertion
// order. `m` is rooted for the duration of the walk, so the callback may
// allocate and run user code.
using MapEntryFn = void (*)(proto::ProtoContext* ctx, void* self,
                            const proto::ProtoObject* key,
                            const proto::ProtoObject* value);
void mapForEach(proto::ProtoContext* ctx, const MapLayout& layout,
                const proto::ProtoObject* m, void* self, MapEntryFn fn);

// Number of entries in map `m` (nullptr is the empty map).
unsigned long mapCount(proto::ProtoContext* ctx, const MapLayout& layout,
                       const proto::ProtoObject* m);

// Value equality of two maps, as used by `=`: true when both hold the same
// number of entries and every key of `a` is present in `b` with a value for
// which `valueEq(ctx, self, valueInA, valueInB)` holds. Insertion order and
// sequence numbers are ignored. Keys are matched as in mapGet (key hash,
// then valuesEqual). nullptr is the empty map.
//
// Cost: O(N log N) — each key of `a` is probed in `b` by the hash already
// stored in `a`'s index, without rehashing; the walk stops calling
// `valueEq` after the first mismatch. Allocates nothing itself; both maps
// must be rooted by the caller if `valueEq` allocates.
using MapValueEqFn = bool (*)(proto::ProtoContext* ctx, void* self,
                              const proto::ProtoObject* a,
                              const proto::ProtoObject* b);
bool mapEquals(proto::ProtoContext* ctx, const MapLayout& layout,
               const proto::ProtoObject* a, const proto::ProtoObject* b,
               void* self, MapValueEqFn valueEq);

// Call `fn(ctx, self, keyHash, value)` for every entry of map `m`, where
// `keyHash` is the key's hash as stored in the hash index (the layout's key
// hash; no rehashing). The order is unspecified (hash-index order). Used by
// valueHash to hash a map independently of insertion order.
//
// Cost: O(N). Allocates nothing itself; `m` must be rooted by the caller if
// `fn` allocates.
using MapHashedEntryFn = void (*)(proto::ProtoContext* ctx, void* self,
                                  unsigned long keyHash,
                                  const proto::ProtoObject* value);
void mapForEachHashedEntry(proto::ProtoContext* ctx, const MapLayout& layout,
                           const proto::ProtoObject* m, void* self,
                           MapHashedEntryFn fn);

} // namespace protoClojure
