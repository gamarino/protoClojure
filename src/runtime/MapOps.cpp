#include "MapOps.h"

#include "protoCore.h"

#include <stdexcept>

namespace protoClojure {

namespace {

// Positions inside the state list.
constexpr int kStateOrder   = 0;
constexpr int kStateIndex   = 1;
constexpr int kStateNextSeq = 2;

// A bucket is a flat ProtoList of (key value seq) triples.
constexpr unsigned long kTriple = 3;

// Automatic-local slots of a MapBuilder scope.
constexpr unsigned int kSlotOrder  = 0;
constexpr unsigned int kSlotIndex  = 1;
constexpr unsigned int kSlotTmp    = 2;
constexpr unsigned int kSlotResult = 3;
constexpr unsigned int kSlotCount  = 4;

inline const proto::ProtoSparseList* asSparse(const proto::ProtoObject* o) {
    return reinterpret_cast<const proto::ProtoSparseList*>(o);
}

inline unsigned long keyHash(proto::ProtoContext* ctx, const MapLayout& layout,
                             const proto::ProtoObject* key) {
    return layout.hash ? layout.hash(ctx, key) : key->getHash(ctx);
}

// The state list of `m`, or nullptr for a map that has never held an entry.
const proto::ProtoList* stateOf(proto::ProtoContext* ctx,
                                const MapLayout& layout,
                                const proto::ProtoObject* m) {
    if (!m || m == PROTO_NONE) return nullptr;
    const proto::ProtoObject* st = m->getAttribute(ctx, layout.stateKey);
    if (!st || st == PROTO_NONE) return nullptr;
    return st->asList(ctx);
}

// The bucket stored under `hash` in `index`, or nullptr.
const proto::ProtoList* bucketAt(proto::ProtoContext* ctx,
                                 const proto::ProtoSparseList* index,
                                 unsigned long hash) {
    if (!index->has(ctx, hash)) return nullptr;
    const proto::ProtoObject* b = index->getAt(ctx, hash);
    if (!b || b == PROTO_NONE) return nullptr;
    return b->asList(ctx);
}

// Position of the triple holding `key` inside a bucket, or -1.
long bucketFind(proto::ProtoContext* ctx, const proto::ProtoList* bucket,
                const proto::ProtoObject* key) {
    unsigned long n = bucket->getSize(ctx);
    for (unsigned long i = 0; i < n; i += kTriple) {
        if (bucket->getAt(ctx, static_cast<int>(i))->compare(ctx, key) == 0)
            return static_cast<long>(i);
    }
    return -1;
}

// A working copy of one map's state, rooted in the slots of a child
// context. Construct it on the stack only (contexts nest LIFO).
class MapBuilder {
public:
    MapBuilder(proto::ProtoContext* parent, const MapLayout& layout,
               const proto::ProtoObject* base)
        : scope_(parent->space, parent), layout_(layout) {
        scope_.resizeAutomaticLocals(kSlotCount);
        const proto::ProtoList* st = stateOf(&scope_, layout, base);
        if (st) {
            scope_.setAutomaticLocal(kSlotOrder, st->getAt(&scope_, kStateOrder));
            scope_.setAutomaticLocal(kSlotIndex, st->getAt(&scope_, kStateIndex));
            nextSeq_ = st->getAt(&scope_, kStateNextSeq)->asLong(&scope_);
        } else {
            scope_.setAutomaticLocal(kSlotOrder,
                scope_.newSparseList()->asObject(&scope_));
            scope_.setAutomaticLocal(kSlotIndex,
                scope_.newSparseList()->asObject(&scope_));
            nextSeq_ = 0;
        }
    }

    proto::ProtoContext* ctx() { return &scope_; }

    void assoc(unsigned long hash, const proto::ProtoObject* key,
               const proto::ProtoObject* value) {
        const proto::ProtoSparseList* index = slotSparse(kSlotIndex);
        const proto::ProtoList* bucket = bucketAt(&scope_, index, hash);
        long pos = bucket ? bucketFind(&scope_, bucket, key) : -1;

        if (pos >= 0) {
            // Existing key: replace the value in its triple. The stored key
            // object and the sequence number (so the position) are kept and
            // the order store is untouched.
            scope_.setAutomaticLocal(kSlotTmp,
                bucket->setAt(&scope_, static_cast<int>(pos + 1), value)
                    ->asObject(&scope_));
            setIndexBucket(index, hash);
            return;
        }

        // New key: append at the end of the insertion order.
        long long seq = nextSeq_++;
        scope_.setAutomaticLocal(kSlotOrder,
            slotSparse(kSlotOrder)
                ->setAt(&scope_, static_cast<unsigned long>(seq), key)
                ->asObject(&scope_));

        const proto::ProtoObject* triple[kTriple] = {
            key, value, scope_.fromLong(seq)};
        if (bucket) {
            // Hash collision: extend the bucket. `bucket` stays reachable
            // through the index slot until the index is replaced below.
            scope_.setAutomaticLocal(kSlotTmp,
                bucket->appendLast(&scope_, triple[0])->asObject(&scope_));
            for (unsigned long i = 1; i < kTriple; ++i) {
                scope_.setAutomaticLocal(kSlotTmp,
                    slotList(kSlotTmp)->appendLast(&scope_, triple[i])
                        ->asObject(&scope_));
            }
        } else {
            scope_.setAutomaticLocal(kSlotTmp,
                scope_.newList(kTriple, triple)->asObject(&scope_));
        }
        setIndexBucket(index, hash);
    }

    // Returns false (and changes nothing) when `key` is absent.
    bool dissoc(unsigned long hash, const proto::ProtoObject* key) {
        const proto::ProtoSparseList* index = slotSparse(kSlotIndex);
        const proto::ProtoList* bucket = bucketAt(&scope_, index, hash);
        long pos = bucket ? bucketFind(&scope_, bucket, key) : -1;
        if (pos < 0) return false;

        long long seq =
            bucket->getAt(&scope_, static_cast<int>(pos + 2))->asLong(&scope_);
        scope_.setAutomaticLocal(kSlotOrder,
            slotSparse(kSlotOrder)
                ->removeAt(&scope_, static_cast<unsigned long>(seq))
                ->asObject(&scope_));

        if (bucket->getSize(&scope_) == kTriple) {
            scope_.setAutomaticLocal(kSlotIndex,
                index->removeAt(&scope_, hash)->asObject(&scope_));
            return true;
        }
        scope_.setAutomaticLocal(kSlotTmp,
            bucket->removeAt(&scope_, static_cast<int>(pos))->asObject(&scope_));
        for (unsigned long i = 1; i < kTriple; ++i) {
            scope_.setAutomaticLocal(kSlotTmp,
                slotList(kSlotTmp)->removeAt(&scope_, static_cast<int>(pos))
                    ->asObject(&scope_));
        }
        setIndexBucket(index, hash);
        return true;
    }

    // Publish the state on a fresh wrapper. The result is UNROOTED once
    // this builder goes out of scope (P2 calling convention).
    const proto::ProtoObject* finish() {
        const proto::ProtoObject* state[3] = {
            scope_.getAutomaticLocal(kSlotOrder),
            scope_.getAutomaticLocal(kSlotIndex),
            scope_.fromLong(nextSeq_)};
        scope_.setAutomaticLocal(kSlotTmp,
            scope_.newList(3, state)->asObject(&scope_));
        proto::ProtoObject* wrap = const_cast<proto::ProtoObject*>(
            layout_.marker->newChild(&scope_, /*isMutable=*/true));
        scope_.setAutomaticLocal(kSlotResult, wrap);
        wrap->setAttribute(&scope_, layout_.stateKey,
                           scope_.getAutomaticLocal(kSlotTmp));
        return scope_.getAutomaticLocal(kSlotResult);
    }

private:
    const proto::ProtoSparseList* slotSparse(unsigned int slot) {
        return asSparse(scope_.getAutomaticLocal(slot));
    }
    const proto::ProtoList* slotList(unsigned int slot) {
        return scope_.getAutomaticLocal(slot)->asList(&scope_);
    }
    // index[hash] = the bucket held in kSlotTmp. `index` is the list held
    // in kSlotIndex, which this replaces.
    void setIndexBucket(const proto::ProtoSparseList* index, unsigned long hash) {
        scope_.setAutomaticLocal(kSlotIndex,
            index->setAt(&scope_, hash, scope_.getAutomaticLocal(kSlotTmp))
                ->asObject(&scope_));
    }

    proto::ProtoContext scope_;
    const MapLayout&    layout_;
    long long           nextSeq_ = 0;
};

void requireEven(unsigned long n) {
    if (n % 2 != 0)
        throw std::runtime_error("map: a key has no value");
}

} // namespace

bool isMap(proto::ProtoContext* ctx, const MapLayout& layout,
           const proto::ProtoObject* v) {
    return v && v != PROTO_NONE && v->getPrototype(ctx) == layout.marker;
}

const proto::ProtoObject* mapAssocPairs(proto::ProtoContext* ctx,
                                        const MapLayout& layout,
                                        const proto::ProtoObject* base,
                                        const proto::ProtoObject* const* kv,
                                        unsigned long n) {
    requireEven(n);
    MapBuilder b(ctx, layout, base);
    for (unsigned long i = 0; i < n; i += 2) {
        b.assoc(keyHash(b.ctx(), layout, kv[i]), kv[i], kv[i + 1]);
    }
    return b.finish();
}

const proto::ProtoObject* mapAssocPairs(proto::ProtoContext* ctx,
                                        const MapLayout& layout,
                                        const proto::ProtoObject* base,
                                        const proto::ProtoList* items,
                                        unsigned long from,
                                        unsigned long n) {
    requireEven(n);
    MapBuilder b(ctx, layout, base);
    for (unsigned long i = from; i < from + n; i += 2) {
        const proto::ProtoObject* k = items->getAt(b.ctx(), static_cast<int>(i));
        b.assoc(keyHash(b.ctx(), layout, k), k,
                items->getAt(b.ctx(), static_cast<int>(i + 1)));
    }
    return b.finish();
}

const proto::ProtoObject* mapDissoc(proto::ProtoContext* ctx,
                                    const MapLayout& layout,
                                    const proto::ProtoObject* m,
                                    const proto::ProtoObject* key) {
    if (!stateOf(ctx, layout, m)) return m;
    MapBuilder b(ctx, layout, m);
    if (!b.dissoc(keyHash(b.ctx(), layout, key), key)) return m;
    return b.finish();
}

const proto::ProtoObject* mapGet(proto::ProtoContext* ctx,
                                 const MapLayout& layout,
                                 const proto::ProtoObject* m,
                                 const proto::ProtoObject* key,
                                 bool* found) {
    *found = false;
    const proto::ProtoList* st = stateOf(ctx, layout, m);
    if (!st) return PROTO_NONE;
    const proto::ProtoList* bucket = bucketAt(
        ctx, asSparse(st->getAt(ctx, kStateIndex)), keyHash(ctx, layout, key));
    if (!bucket) return PROTO_NONE;
    long pos = bucketFind(ctx, bucket, key);
    if (pos < 0) return PROTO_NONE;
    *found = true;
    return bucket->getAt(ctx, static_cast<int>(pos + 1));
}

void mapForEach(proto::ProtoContext* ctx, const MapLayout& layout,
                const proto::ProtoObject* m, void* self, MapEntryFn fn) {
    const proto::ProtoList* st = stateOf(ctx, layout, m);
    if (!st) return;
    // Pin the map: a callback may replace the only reference to it (for
    // example remove-watch from inside a watch) and allocate.
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(1);
    scope.setAutomaticLocal(0, m);
    struct Adapter {
        const MapLayout* layout;
        const proto::ProtoSparseList* index;
        void* self;
        MapEntryFn fn;
    } adapter{&layout, asSparse(st->getAt(&scope, kStateIndex)), self, fn};
    asSparse(st->getAt(&scope, kStateOrder))->processElements(
        &scope, &adapter,
        [](proto::ProtoContext* c, void* a, unsigned long /*seq*/,
           const proto::ProtoObject* key) {
            const auto* ad = static_cast<Adapter*>(a);
            const proto::ProtoList* bucket =
                bucketAt(c, ad->index, keyHash(c, *ad->layout, key));
            long pos = bucket ? bucketFind(c, bucket, key) : -1;
            if (pos < 0) return;  // unreachable for a well-formed map
            ad->fn(c, ad->self, key,
                   bucket->getAt(c, static_cast<int>(pos + 1)));
        });
}

unsigned long mapCount(proto::ProtoContext* ctx, const MapLayout& layout,
                       const proto::ProtoObject* m) {
    const proto::ProtoList* st = stateOf(ctx, layout, m);
    return st ? asSparse(st->getAt(ctx, kStateOrder))->getSize(ctx) : 0;
}

bool mapEquals(proto::ProtoContext* ctx, const MapLayout& layout,
               const proto::ProtoObject* a, const proto::ProtoObject* b,
               void* self, MapValueEqFn valueEq) {
    if (a == b) return true;
    const unsigned long n = mapCount(ctx, layout, a);
    if (n != mapCount(ctx, layout, b)) return false;
    if (n == 0) return true;

    // Walk `a`'s hash index, not its order store: the index hands over each
    // bucket together with the hash it is stored under, so the matching
    // bucket of `b` is one sparse-list probe away. Equal counts plus every
    // key of `a` found in `b` means the key sets are equal.
    struct Walk {
        const proto::ProtoSparseList* otherIndex;
        void*                         self;
        MapValueEqFn                  valueEq;
        bool                          equal;
    } walk{asSparse(stateOf(ctx, layout, b)->getAt(ctx, kStateIndex)),
           self, valueEq, true};
    asSparse(stateOf(ctx, layout, a)->getAt(ctx, kStateIndex))->processElements(
        ctx, &walk,
        [](proto::ProtoContext* c, void* p, unsigned long hash,
           const proto::ProtoObject* bucketObj) {
            auto* w = static_cast<Walk*>(p);
            if (!w->equal) return;  // processElements cannot stop early
            const proto::ProtoList* other = bucketAt(c, w->otherIndex, hash);
            if (!other) { w->equal = false; return; }
            const proto::ProtoList* bucket = bucketObj->asList(c);
            const unsigned long size = bucket->getSize(c);
            for (unsigned long i = 0; i < size && w->equal; i += kTriple) {
                long pos = bucketFind(
                    c, other, bucket->getAt(c, static_cast<int>(i)));
                w->equal = pos >= 0 &&
                    w->valueEq(c, w->self,
                               bucket->getAt(c, static_cast<int>(i + 1)),
                               other->getAt(c, static_cast<int>(pos + 1)));
            }
        });
    return walk.equal;
}

} // namespace protoClojure
