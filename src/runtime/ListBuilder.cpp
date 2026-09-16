#include "runtime/ListBuilder.h"

#include "protoCore.h"

namespace protoClojure {

ListBuilder::ListBuilder(proto::ProtoContext* parent)
    : holder_(new proto::ProtoContext(parent->space, parent)) {
    holder_->resizeAutomaticLocals(kSlotPending0 + capacity_);
    holder_->setAutomaticLocal(kSlotAcc, holder_->newList()->asObject(holder_));
}

ListBuilder::~ListBuilder() {
    delete holder_;
}

void ListBuilder::push(const proto::ProtoObject* value) {
    // Root first, allocate second. On entry pending_ < capacity_ always, so
    // this store cannot allocate and `value` — which may be an unrooted
    // pointer the caller has just been handed — reaches a GC-visible slot
    // before any collection can start.
    holder_->setAutomaticLocal(kSlotPending0 + pending_, value);
    ++pending_;
    ++size_;
    if (pending_ < capacity_) return;
    // Full. Either widen the parking area (cheap, allocates no cell) or, once
    // it is at its maximum, fold the parked elements into the accumulator and
    // hand that chunk's garbage to the collector.
    if (capacity_ < kMaxPending) {
        capacity_ = (capacity_ * 4 < kMaxPending) ? capacity_ * 4 : kMaxPending;
        holder_->resizeAutomaticLocals(kSlotPending0 + capacity_);
    } else {
        flush();
    }
}

void ListBuilder::flush() {
    if (pending_ == 0) return;
    {
        // Every append below allocates through `chunk`. When it destructs,
        // `~ProtoContext` submits its young generation to the collector: the
        // log2(N) cells each append left behind become reclaimable, while the
        // accumulator itself stays rooted in holder_'s slot 0 and survives.
        // This is the whole point of the type — see ListBuilder.h.
        proto::ProtoContext chunk(holder_->space, holder_);
        for (unsigned int i = 0; i < pending_; ++i) {
            const proto::ProtoList* cur =
                holder_->getAutomaticLocal(kSlotAcc)->asList(&chunk);
            const proto::ProtoList* grown =
                cur->appendLast(&chunk,
                                holder_->getAutomaticLocal(kSlotPending0 + i));
            holder_->setAutomaticLocal(kSlotAcc, grown->asObject(&chunk));
        }
    }
    // Drop the parked references: a stale slot would keep an element alive
    // long after the accumulator is the only thing that should hold it.
    for (unsigned int i = 0; i < pending_; ++i) {
        holder_->setAutomaticLocal(kSlotPending0 + i, PROTO_NONE);
    }
    pending_ = 0;
}

const proto::ProtoObject* ListBuilder::finish() {
    flush();
    const proto::ProtoObject* result = holder_->getAutomaticLocal(kSlotAcc);
    // Anchor the result in the parent's young generation: ~ProtoContext turns
    // a context's returnValue into a ReturnReference held by `previous`, so
    // the list stays reachable across the window between this return and the
    // caller storing it in a slot of its own.
    holder_->returnValue = result;
    return result;
}

const proto::ProtoList* ListBuilder::finishList() {
    return finish()->asList(holder_);
}

} // namespace protoClojure
