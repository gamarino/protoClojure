#include "VectorOps.h"

#include "protoCore.h"

namespace protoClojure {

namespace {

// The single index the box stores its elements under.
constexpr unsigned long kItemsSlot = 0;

inline const proto::ProtoSparseList* asBox(const proto::ProtoObject* v) {
    // isVector has already checked the tag, so the cast is the whole
    // conversion.
    return reinterpret_cast<const proto::ProtoSparseList*>(v);
}

} // namespace

const proto::ProtoObject* newVector(proto::ProtoContext* ctx,
                                    const proto::ProtoList* items) {
    // Two allocations, both Small-form cells: the empty box and the box with
    // the entry. The empty one is a young cell of `ctx` for the length of the
    // setAt, so nothing here needs a slot of its own.
    return ctx->newSparseList()
        ->setAt(ctx, kItemsSlot, items->asObject(ctx))
        ->asObject(ctx);
}

const proto::ProtoList* vectorItems(proto::ProtoContext* ctx,
                                    const proto::ProtoObject* v) {
    return asBox(v)->getAt(ctx, kItemsSlot)->asList(ctx);
}

} // namespace protoClojure
