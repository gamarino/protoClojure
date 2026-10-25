#include "CallerSlots.h"

#include "protoCore.h"

#include <stdexcept>

namespace protoClojure {

namespace {
thread_local PrimitiveWindow* t_window = nullptr;

// A young list cell of `ctx` referencing `v`: live until `ctx`'s chain is
// submitted.
void anchorIn(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
    if (!v || !proto::ProtoObject::isCellPointer(v)) return;
    (void) ctx->newList()->appendLast(ctx, v);
}
} // namespace

PrimitiveWindow*& currentPrimitiveWindow() { return t_window; }

SafepointPermit::SafepointPermit(proto::ProtoContext* ctx) : w_(t_window) {
    if (w_ && w_->frame == ctx && w_->depth == 1) ++w_->permits;
    else w_ = nullptr;
}

SafepointPermit::~SafepointPermit() {
    if (w_) --w_->permits;
}

CallerSlots::CallerSlots(proto::ProtoContext* ctx, unsigned int n)
    : ctx_(ctx), window_(t_window), slots_(local_), n_(n) {
    if (n > kMaxSlots) throw std::logic_error("CallerSlots: more than kMaxSlots slots");
    if (window_) {
        submissionsAtStart_ = window_->submissions;
        if (static_cast<unsigned int>(window_->end - window_->next) >= n) {
            slots_ = window_->next;
            window_->next += n;
            inWindow_ = true;
        } else {
            ++window_->local;
        }
    }
    for (unsigned int i = 0; i < n; ++i) slots_[i] = PROTO_NONE;
}

CallerSlots::~CallerSlots() {
    for (unsigned int i = 0; i < n_; ++i) slots_[i] = nullptr;
    if (inWindow_) window_->next = slots_;
    else if (window_) --window_->local;
}

void CallerSlots::pin(unsigned int i, const proto::ProtoObject* v) {
    slots_[i] = v;
    if (!inWindow_) anchorIn(ctx_, v);
}

void CallerSlots::safepoint() {
    if (inWindow_ && window_ == t_window && window_->frame == ctx_ && window_->depth == 1 &&
        window_->permits > 0 && window_->local == 0) {
        ++window_->submissions;
        ctx_->safepoint();
    }
}

const proto::ProtoObject* CallerSlots::ret(const proto::ProtoObject* v) {
    if (window_ && window_->submissions != submissionsAtStart_) anchorIn(ctx_, v);
    return v;
}

} // namespace protoClojure
