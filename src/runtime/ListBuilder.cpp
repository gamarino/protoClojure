#include "runtime/ListBuilder.h"
#include "runtime/SeqTypes.h"

#include "protoCore.h"

namespace protoClojure {

ListBuilder::ListBuilder(proto::ProtoContext* ctx)
    : ctx_(ctx), slots_(ctx, 1 + kMaxPending) {}

ListBuilder::~ListBuilder() = default;

const proto::ProtoList* ListBuilder::acc() {
    return slots_.get(kSlotAcc) == PROTO_NONE ? ctx_->newList()
                                              : slots_.get(kSlotAcc)->asList(ctx_);
}

void ListBuilder::push(const proto::ProtoObject* value) {
    ++size_;
    if (!slots_.traced()) {
        // Untraced slots: appended at once, so the element is referenced by a
        // young cell of ctx_ before anything else can run.
        slots_.set(kSlotAcc, acc()->appendLast(ctx_, value)->asObject(ctx_));
        return;
    }
    slots_.set(kSlotPending0 + npending_++, value);
    if (npending_ == kMaxPending) {
        flush();
        // Nothing is parked: the folded chunk's garbage may go now, when the
        // primitive at the bottom holds a SafepointPermit (protoCore submits
        // once the context is past its threshold).
        slots_.safepoint();
    }
}

void ListBuilder::flush() {
    if (npending_ == 0) return;
    const proto::ProtoList* cur = acc();
    for (unsigned int i = 0; i < npending_; ++i) {
        cur = cur->appendLast(ctx_, slots_.get(kSlotPending0 + i));
        slots_.set(kSlotAcc, cur->asObject(ctx_));
    }
    for (unsigned int i = 0; i < npending_; ++i) slots_.set(kSlotPending0 + i, PROTO_NONE);
    npending_ = 0;
}

const proto::ProtoObject* ListBuilder::finish() {
    flush();
    if (slots_.get(kSlotAcc) == PROTO_NONE) slots_.set(kSlotAcc, ctx_->newList()->asObject(ctx_));
    return slots_.ret(slots_.get(kSlotAcc));
}

const proto::ProtoList* ListBuilder::finishList() {
    return finish()->asList(ctx_);
}

const proto::ProtoObject* ListBuilder::finishSeq() {
    const proto::ProtoObject* result;
    if (slots_.traced() && size_ == npending_) {
        // Nothing was folded (at most kMaxPending elements): the cells are
        // built straight from the parked elements, rooted in their slots.
        result = listFromArray(ctx_, npending_, slots_.data() + kSlotPending0);
    } else {
        flush();
        result = listFromProtoList(ctx_, acc());
    }
    for (unsigned int i = 0; i < npending_; ++i) slots_.set(kSlotPending0 + i, PROTO_NONE);
    npending_ = 0;
    slots_.set(kSlotAcc, result);
    return slots_.ret(result);
}

} // namespace protoClojure
