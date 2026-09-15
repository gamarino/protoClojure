/*
 * MapOps — the single implementation of protoClojure's runtime map.
 *
 * Representation. A map IS an immutable protoCore ProtoSparseList, with no
 * wrapper object and no mutable state. Each entry is indexed by the pointer
 * bits of its key's canonical key (canonicalKey below), and the value under
 * that index is a 2-element ProtoList (original-key value): the key object
 * the entry was created with, and the entry's value.
 *   - count is ProtoSparseList::getSize: O(1);
 *   - get and contains? canonicalize the key, then one getAt: O(log n);
 *   - assoc is getAt plus setAt, dissoc is getAt plus removeAt: O(log n);
 *   - assoc of a key already present keeps the stored key object and
 *     replaces the value, and returns the same map when the new value is the
 *     identical object;
 *   - walks (printing, keys, vals, =) visit the entries in the sparse list's
 *     ascending index order. The indices are memory addresses, so that order
 *     is unspecified and can differ between runs of the same program; keys
 *     and vals of one map value still line up.
 * A ProtoSparseList holds at most 2^24 - 1 entries.
 *
 * Canonical keys. Keys that name the same entry share one canonical key
 * pointer:
 *   - SmallIntegers, booleans and nil (embedded values): the value itself;
 *   - keywords and symbols (interned Named values, Named.h): themselves;
 *   - strings: the protoCore symbol of their content (createSymbol), which is
 *     unique by content. For up to 6 ASCII bytes that symbol is the inline
 *     string itself and costs no allocation. Every other string is copied
 *     into the symbol table (see "Memory and cost");
 *   - vectors: themselves when every element is its own canonical key
 *     (protoCore interns every ProtoTuple by its elements), otherwise the
 *     tuple of the canonical keys of the elements;
 *   - lists: the tuple of the canonical keys of their elements, so a list and
 *     a vector with equal elements are one key;
 *   - doubles: the tuple (double-marker hi lo), the 64-bit pattern split into
 *     two 32-bit SmallIntegers. -0.0 and 0.0 are different keys; a NaN finds
 *     only a NaN with the same bit pattern;
 *   - integers beyond the SmallInteger range (LargeInteger): the tuple
 *     (bigint-marker sign limb0 limb1 ...), with sign 1 for a negative value
 *     and 0 otherwise, and the magnitude's hexadecimal digits (protoCore
 *     asIntegerString, base 16) in limbs of 13 digits, least significant
 *     first. protoCore never stores a value in the SmallInteger range as a
 *     LargeInteger, so SmallInteger keys need no special case;
 *   - maps: the tuple (map-marker ck1 cv1 ck2 cv2 ...) of their entries'
 *     canonical keys and the canonical keys of their values, in ascending
 *     index order, which is the same for every map with the same key set;
 *   - anything else (atoms, functions, futures, promises, actors): itself.
 * Numbers of different types are different keys, as in JVM Clojure, even
 * though `=` holds between them (deviation D15): 1 and 1.0, 0 and -0.0, and
 * a big integer and an equal double. The rules apply at every depth of a
 * collection key.
 *
 * The three markers (MapKeyMarkers) are private immutable objects, never
 * keywords and never in the Named table. Canonical keys never reach user
 * code, which sees only original keys, so no user value can be pointer-equal
 * to a marker tuple.
 *
 * Memory and cost. Canonical keys are interned symbols and tuples, which
 * protoCore never frees: a key's canonical form stays in memory until the
 * program ends. Canonicalizing is O(1) for embedded values, keywords and
 * symbols, O(length) for other strings and doubles and big integers, and
 * proportional to the total size of a collection key, on every operation
 * that takes a key.
 *
 * GC rooting (engineering principles P1/P2): builders keep intermediates in
 * automatic locals of a child ProtoContext. Returned maps are UNROOTED; the
 * caller roots them before its next allocation. Maps, keys and values passed
 * in must be rooted by the caller. A canonical key is interned (never
 * collected) or is the key itself.
 */
#pragma once

namespace proto {
class ProtoContext;
class ProtoObject;
class ProtoList;
}

namespace protoClojure {

// The private first slots of the marker tuples of doubles, big integers and
// maps. Rooted by the runtime for the lifetime of the ProtoSpace and carried
// in the ActiveCallContext (Primitives.h).
struct MapKeyMarkers {
    const proto::ProtoObject* doubleKey = nullptr;
    const proto::ProtoObject* bigIntegerKey = nullptr;
    const proto::ProtoObject* mapKey = nullptr;
};

// True when `v` is a map: a protoCore sparse list in its AVL or its small
// form (pointer tags 8 and 26 of protoCore's headers/proto_internal.h). The
// runtime never hands a sparse list to user code for anything else.
inline bool isMap(const proto::ProtoObject* v) {
    constexpr unsigned long kTagMask = 0x3F;
    constexpr unsigned long kTagSparseList = 8;
    constexpr unsigned long kTagSparseListSmall = 26;
    const unsigned long tag = reinterpret_cast<unsigned long>(v) & kTagMask;
    return v != nullptr && (tag == kTagSparseList || tag == kTagSparseListSmall);
}

// The canonical key of `key` (rules above). nullptr is nil. May allocate
// interned symbols and tuples; `key` must be rooted by the caller.
const proto::ProtoObject* canonicalKey(proto::ProtoContext* ctx,
                                       const MapKeyMarkers& markers,
                                       const proto::ProtoObject* key);

// Return `base` (a map, or nullptr for the empty map) with `n` alternating
// keys and values from `kv` associated in order; a repeated key keeps its
// first key object and its last value. `n` must be even.
const proto::ProtoObject* mapAssocPairs(proto::ProtoContext* ctx,
                                        const MapKeyMarkers& markers,
                                        const proto::ProtoObject* base,
                                        const proto::ProtoObject* const* kv,
                                        unsigned long n);

// Same, taking the `n` keys and values from `items[from .. from+n)`.
const proto::ProtoObject* mapAssocPairs(proto::ProtoContext* ctx,
                                        const MapKeyMarkers& markers,
                                        const proto::ProtoObject* base,
                                        const proto::ProtoList* items,
                                        unsigned long from,
                                        unsigned long n);

// Return `m` without `key`. Returns `m` itself when the key is absent.
const proto::ProtoObject* mapDissoc(proto::ProtoContext* ctx,
                                    const MapKeyMarkers& markers,
                                    const proto::ProtoObject* m,
                                    const proto::ProtoObject* key);

// Look `key` up in map `m` (nullptr is the empty map). Sets `*found` and
// returns the value, or PROTO_NONE when absent.
const proto::ProtoObject* mapGet(proto::ProtoContext* ctx,
                                 const MapKeyMarkers& markers,
                                 const proto::ProtoObject* m,
                                 const proto::ProtoObject* key,
                                 bool* found);

// Number of entries in map `m` (nullptr is the empty map). O(1).
unsigned long mapCount(proto::ProtoContext* ctx, const proto::ProtoObject* m);

// Call `fn(ctx, self, key, value)` for every entry of map `m`, with the
// original key, in unspecified order. `m` is pinned for the duration of the
// walk. On maps of more than 3 entries protoCore runs the walk inside a GC
// critical section, so `fn` may build structures but must not run user code
// (collect the entries first, as fireWatches does).
using MapEntryFn = void (*)(proto::ProtoContext* ctx, void* self,
                            const proto::ProtoObject* key,
                            const proto::ProtoObject* value);
void mapForEach(proto::ProtoContext* ctx, const proto::ProtoObject* m,
                void* self, MapEntryFn fn);

// Value equality of two maps, as used by `=`: true when both hold the same
// number of entries and every entry of `a` has an entry of `b` under the same
// canonical key whose value satisfies `valueEq(ctx, self, valueInA,
// valueInB)`. nullptr is the empty map. Walks `a` and probes `b`, O(n log n);
// allocates nothing itself. The same critical-section rule as mapForEach
// applies to `valueEq`.
using MapValueEqFn = bool (*)(proto::ProtoContext* ctx, void* self,
                              const proto::ProtoObject* a,
                              const proto::ProtoObject* b);
bool mapEquals(proto::ProtoContext* ctx,
               const proto::ProtoObject* a, const proto::ProtoObject* b,
               void* self, MapValueEqFn valueEq);

} // namespace protoClojure
