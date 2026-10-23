/*
 * SetOps — the representation of a protoClojure set (docs/DESIGN.md §10.2,
 * M24).
 *
 * A set is an immutable object whose prototype is the set prototype and
 * whose one own attribute, `__set__`, holds a protoCore `ProtoMap` driven
 * through the same hashed-collection helper and key semantics as a map
 * (MapOps.h), mapping each element to itself. So an element is matched as a
 * map key is: keywords, symbols and other identity values by pointer,
 * strings, numbers and collections by value, numbers of different types as
 * different elements (`#{1 1.0}` has two). The stored element is the one
 * the set returns: `(get #{[1 2]} '(1 2))` answers the vector.
 *
 * Why a wrapper object and not a bare ProtoMap: a set must be told from a
 * map in O(1), and protoClojure cannot mint a protoCore pointer tag. The
 * vector's trick (a one-entry sparse list, VectorOps.h) has no free tag left
 * that the collector traces and protoCore does not intern; protoCore's own
 * ProtoSet keys elements by a 64-bit hash with no collision bucket, so two
 * different elements with one hash would be one element. The object costs
 * one cell and one own-attribute read per operation (protoCore's attribute
 * cache), the same shape protoScala uses for its sets.
 *
 * Costs: conj / disj / contains? / get are one keyHash plus O(log n), like a
 * map; count is O(n) (D25); walks are in unspecified order (D24).
 *
 * The prototype is created once per ProtoSpace by initSetOps, before any
 * set is built (installPrimitives calls it), and must be rooted by the
 * caller (it is stored on the globals). GC rooting otherwise follows
 * MapOps.h: inputs rooted by the caller, results unrooted.
 */
#pragma once

#include "protoCore.h"  // proto::proto_ulong

namespace proto {
class ProtoContext;
class ProtoObject;
}

namespace protoClojure {

// Creates the set prototype of this ProtoSpace and returns it; the caller
// roots it. Calling it again (a new ProtoSpace) replaces the previous one.
const proto::ProtoObject* initSetOps(proto::ProtoContext* ctx);

// The set prototype of the current ProtoSpace (the VM compares a callable's
// prototype with it).
const proto::ProtoObject* setPrototype();

// True when `v` is a set. O(1): a pointer-tag test and a prototype read.
bool isSet(proto::ProtoContext* ctx, const proto::ProtoObject* v);

// The ProtoMap of set `s` (element -> element), or nullptr when it is empty.
const proto::ProtoObject* setElements(proto::ProtoContext* ctx, const proto::ProtoObject* s);

// `base` (a set, or nullptr for the empty set) with the `n` elements of
// `items` added; an element already present is kept, not replaced.
const proto::ProtoObject* setConj(proto::ProtoContext* ctx, const proto::ProtoObject* base,
                                  const proto::ProtoObject* const* items, proto::proto_ulong n);

// `s` without `item`; `s` itself when `item` is absent.
const proto::ProtoObject* setDisj(proto::ProtoContext* ctx, const proto::ProtoObject* s,
                                  const proto::ProtoObject* item);

// The stored element equal to `item`, with `*found` set; PROTO_NONE when absent.
const proto::ProtoObject* setGet(proto::ProtoContext* ctx, const proto::ProtoObject* s,
                                 const proto::ProtoObject* item, bool* found);

// Number of elements, O(n).
proto::proto_ulong setCount(proto::ProtoContext* ctx, const proto::ProtoObject* s);

// True when `s` has no element. O(1).
bool setIsEmpty(proto::ProtoContext* ctx, const proto::ProtoObject* s);

// Calls `fn(ctx, self, element)` for each element, in unspecified order;
// `s` is pinned for the walk and `fn` may allocate.
using SetElementFn = void (*)(proto::ProtoContext* ctx, void* self,
                              const proto::ProtoObject* element);
void setForEach(proto::ProtoContext* ctx, const proto::ProtoObject* s, void* self,
                SetElementFn fn);

// True when `a` and `b` hold the same elements (each matched as a key).
bool setEquals(proto::ProtoContext* ctx, const proto::ProtoObject* a,
               const proto::ProtoObject* b);

// An order-independent hash of the elements, consistent with setEquals.
proto::proto_ulong setHash(proto::ProtoContext* ctx, const proto::ProtoObject* s);

} // namespace protoClojure
