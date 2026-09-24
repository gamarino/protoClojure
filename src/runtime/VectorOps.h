/*
 * VectorOps — the representation of a protoClojure vector.
 *
 * A vector is an immutable protoCore `ProtoSparseList` holding exactly ONE
 * entry, under index 0, whose value is the `ProtoList` of the vector's
 * elements. Nothing else in the runtime ever hands a sparse list to user
 * code, so the pointer tag of that one-entry box is what tells a vector from
 * a list in O(1) — which is the whole reason the box exists.
 *
 * Why not a bare ProtoList (decision R2, CHANGELOG). A vector wants exactly
 * what `ProtoList` is: an immutable AVL list with structural sharing, an
 * O(N) bulk constructor, O(log N) indexed access and ordinary garbage
 * collection. What it cannot have is `ProtoList`'s pointer tag, because a
 * Clojure *list* is already a `ProtoList` and the two must stay
 * distinguishable: `vector?`, `list?`, `conj` and printing (`[1 2]` against
 * `(1 2)`) all depend on it, and protoClojure cannot mint a protoCore tag of
 * its own. The one-entry sparse list is the cheapest immutable box that
 * carries a distinct tag: one cell per vector (the Small inline form, which
 * a one-entry sparse list never outgrows), an inline scan of up to three
 * slots per unboxing, no wrapper object, no attribute lookup, no entry in
 * protoCore's mutables tree. The tag it uses is the one protoClojure's maps
 * vacated when they moved to `ProtoMap`.
 *
 * What changed against the previous representation, an interned `ProtoTuple`:
 *   - a vector is COLLECTED when the last reference to it dies. protoCore
 *     interns every tuple node and never frees one, so every vector a program
 *     ever built used to stay in memory until the process exited (protoScala
 *     DESIGN risk R2). That is what this representation is for.
 *   - `(vector …)`, `vec` of a list and every seq operation on a vector are
 *     O(1) where they were O(N): the elements already are a `ProtoList`, so
 *     building a vector from the call's argument list is a box, and
 *     `asSeqOrNull` is an unbox instead of a tuple-to-list conversion.
 *   - `nth` is O(log2 N) where it was O(log4 N) — a `ProtoList` node holds
 *     one element and a `ProtoTuple` node four — so a vector of a million
 *     elements takes about twice as many node hops per random access, and a
 *     large vector costs about three times the cells. That is the price of
 *     not retaining every vector for ever, and it is paid only by indexed
 *     access: sequential walks got cheaper.
 *   - equal vectors are no longer pointer-equal. Interning made
 *     `(= [1 2] [1 2])` a pointer comparison; it is now an element-by-element
 *     walk, like every other sequential comparison (`valuesEqual`).
 *
 * GC rooting: `newVector` allocates, so its `items` must be rooted by the
 * caller; the vector it returns is UNROOTED, like every other value a
 * primitive builds.
 */
#pragma once

namespace proto {
class ProtoContext;
class ProtoObject;
class ProtoList;
}

namespace protoClojure {

// True when `v` is a vector: the one-entry sparse-list box above, in either
// of protoCore's forms (pointer tags 8 and 26 of headers/proto_internal.h;
// one entry always fits the Small form, but both are accepted).
inline bool isVector(const proto::ProtoObject* v) {
    constexpr unsigned long kTagMask            = 0x3F;
    constexpr unsigned long kTagSparseList      = 8;
    constexpr unsigned long kTagSparseListSmall = 26;
    const unsigned long tag = reinterpret_cast<unsigned long>(v) & kTagMask;
    return v != nullptr && (tag == kTagSparseList || tag == kTagSparseListSmall);
}

// The vector holding `items`, in that order. O(1): the list is stored as it
// is, not copied.
const proto::ProtoObject* newVector(proto::ProtoContext* ctx,
                                    const proto::ProtoList* items);

// The elements of vector `v` (`isVector` must hold). O(1).
const proto::ProtoList* vectorItems(proto::ProtoContext* ctx,
                                    const proto::ProtoObject* v);

} // namespace protoClojure
