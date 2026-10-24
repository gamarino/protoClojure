#include "LazySeq.h"

#include "Exceptions.h"
#include "ExecutionEngine.h"
#include "Macros.h"
#include "MapOps.h"
#include "Primitives.h"
#include "SetOps.h"
#include "StackGuard.h"
#include "VectorOps.h"

#include "protoCore.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace protoClojure {

namespace {

using PO = proto::ProtoObject;

// Layouts. A cons and a chunk are protoCore lazy objects (children of their
// prototype, SeqTypes.h) published at birth, whose value is a small list of
// their fields: [first more] for a cons, [items off end more] for a chunk,
// `end` negated (as -end - 1) for a chunked one. A native step, the producer
// of a lazy seq, is a list [kind a b c d e] (fewer operands when the last
// ones are unused). A list of up to five elements is one cell, built in one
// allocation (ProtoContext::newList(n, items)), and a lazy object three, so
// a cons costs four cells and leaves no garbage.
struct StepEntry {
    StepRealizer fn = nullptr;
    bool pure = false;
};
StepEntry g_steps[static_cast<int>(StepKind::Count_)];

// Lazy seqs this thread is realising, innermost last: a lazy seq found claimed
// that is on this list is being realised by its own thunk.
thread_local std::vector<const PO*> tl_realizing;

constexpr unsigned int kTagList      = 2;
constexpr unsigned int kTagListSmall = 25;
// Elements a chunked step processes together, as Clojure's chunks.
constexpr long long kChunk = 32;

inline bool isListTag(const PO* v) {
    if (!v) return false;
    const auto t = static_cast<unsigned int>(reinterpret_cast<uintptr_t>(v) & 0x3F);
    return t == kTagList || t == kTagListSmall;
}

inline bool truthy(const PO* v) { return v && v != PROTO_NONE && v != PROTO_FALSE; }
inline const PO* orNil(const PO* v) { return v ? v : PROTO_NONE; }

// The fields of a cons or a chunk.
const proto::ProtoList* fieldsOf(proto::ProtoContext* ctx, const PO* node) {
    return node->getLazyValue(ctx)->asList(ctx);
}
const PO* consFirst(proto::ProtoContext* ctx, const PO* c) { return orNil(fieldsOf(ctx, c)->getAt(ctx, 0)); }
const PO* consMore(proto::ProtoContext* ctx, const PO* c)  { return orNil(fieldsOf(ctx, c)->getAt(ctx, 1)); }

// A lazy object of prototype `proto` published at once with `fields`.
const PO* newNode(proto::ProtoContext* ctx, const PO* proto, unsigned n, const PO* const* fields) {
    const PO* node = proto->newLazyChild(ctx, PROTO_NONE);
    node->publishLazy(ctx, ctx->newList(n, fields)->asObject(ctx));
    return node;
}

long long longOf(proto::ProtoContext* ctx, const PO* v) {
    return v->asLong(ctx);
}

const PO* emptyList(proto::ProtoContext* ctx) { return ctx->newList()->asObject(ctx); }

const PO* call(proto::ProtoContext* ctx, const PO* f, const PO* const* args, unsigned n) {
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throwError(ctx, "lazy seq: no active VM context");
    return cc->engine->invoke(ctx, f, args, n);
}
const PO* call1(proto::ProtoContext* ctx, const PO* f, const PO* x) {
    const PO* one[1] = {x};
    return call(ctx, f, one, 1);
}

// A context of the caller's that roots a few values in its slots and hands
// its garbage to the collector every kStride iterations of a loop
// (ProtoContext::safepoint submits once past protoCore's per-context
// threshold). Every value the loop still needs must be in a slot when
// tick() runs.
class Scope {
public:
    Scope(proto::ProtoContext* ctx, unsigned slots) : c_(ctx->space, ctx) {
        c_.resizeAutomaticLocals(slots);
    }
    proto::ProtoContext* ctx() { return &c_; }
    void set(unsigned i, const PO* v) { c_.setAutomaticLocal(i, orNil(v)); }
    const PO* get(unsigned i) { return orNil(c_.getAutomaticLocal(i)); }
    void tick() {
        if (++n_ % kStride == 0) c_.safepoint();
    }
    // Anchors `v` in the parent context when this scope ends, for a result
    // the caller will root.
    const PO* ret(const PO* v) { c_.returnValue = v; return v; }

private:
    static constexpr unsigned kStride = 64;
    proto::ProtoContext c_;
    unsigned n_ = 0;
};

const PO* newStep(proto::ProtoContext* ctx, StepKind kind, const PO* a, const PO* b,
                  const PO* c, const PO* d, const PO* e) {
    const PO* vals[6] = {proto::makeSmallInt(static_cast<int>(kind)), orNil(a), orNil(b),
                         orNil(c), orNil(d), orNil(e)};
    unsigned n = 6;
    while (n > 1 && !(n == 2 ? a : n == 3 ? b : n == 4 ? c : n == 5 ? d : e)) --n;
    return ctx->newList(n, vals)->asObject(ctx);
}

const PO* lazy5(proto::ProtoContext* ctx, StepKind kind, const PO* a, const PO* b,
                const PO* c, const PO* d, const PO* e) {
    Scope s(ctx, 1);
    s.set(0, newStep(s.ctx(), kind, a, b, c, d, e));
    return s.ret(newLazySeq(s.ctx(), s.get(0)));
}

bool isStep(proto::ProtoContext*, const PO* v) { return isListTag(v); }

// Chunk fields.
struct ChunkView {
    const proto::ProtoList* items;
    long long off, end;
    bool chunked;
    const PO* more;
};
ChunkView chunkOf(proto::ProtoContext* ctx, const PO* c) {
    ChunkView v;
    const proto::ProtoList* f = fieldsOf(ctx, c);
    v.items = f->getAt(ctx, 0)->asList(ctx);
    v.off = proto::asSmallInt(f->getAt(ctx, 1));
    const long long e = proto::asSmallInt(f->getAt(ctx, 2));
    v.chunked = e < 0;
    v.end = e < 0 ? -e - 1 : e;
    v.more = orNil(f->getAt(ctx, 3));
    return v;
}

// Runs a thunk: a step through its realiser, a Clojure fn by calling it and
// taking the seq of what it returned (a lazy seq returned by the body is
// realised in turn, as Clojure's LazySeq.sval unwraps nested lazy seqs).
const PO* runThunk(proto::ProtoContext* ctx, const PO* self, const PO* thunk) {
    if (isStep(ctx, thunk)) {
        const proto::ProtoList* st = thunk->asList(ctx);
        const int k = static_cast<int>(proto::asSmallInt(st->getAt(ctx, 0)));
        if (k < 0 || k >= static_cast<int>(StepKind::Count_) || !g_steps[k].fn)
            throwError(ctx, "lazy seq: unknown step");
        StepOperands ops{orNil(st->getAt(ctx, 1)), orNil(st->getAt(ctx, 2)), orNil(st->getAt(ctx, 3)),
                         orNil(st->getAt(ctx, 4)), orNil(st->getAt(ctx, 5))};
        return g_steps[k].fn(ctx, self, ops);
    }
    Scope s(ctx, 1);
    s.set(0, call(s.ctx(), thunk, nullptr, 0));
    return s.ret(seqOf(s.ctx(), s.get(0)));
}

bool thunkIsPure(proto::ProtoContext* ctx, const PO* thunk) {
    if (!isStep(ctx, thunk)) return false;
    const int k = static_cast<int>(proto::asSmallInt(thunk->asList(ctx)->getAt(ctx, 0)));
    return k >= 0 && k < static_cast<int>(StepKind::Count_) && g_steps[k].pure;
}

// Waits while another thread realises a seq. ProtoThread::yield / sleepFor
// take this thread out of the stop-the-world quorum for the wait, so a
// collection the realising thread needs is never held up by the waiter
// (protoCore 3.1.0; the caller holds nothing in C++ locals only).
void waitABit(proto::ProtoContext* ctx, unsigned round) {
    if (round < 64) proto::ProtoThread::yield(ctx);
    else proto::ProtoThread::sleepFor(ctx, round < 1024 ? 50 : 1000);
}

// What follows the seq `s` once `tail` is appended to it: concat's step for
// a non-empty seq, keeping a chunk a chunk.
const PO* prependSeq(proto::ProtoContext* ctx, const PO* s, const PO* restColls);

// ---- steps --------------------------------------------------------------------

// A chunked source's next chunk: the elements [off, min(off + 32, end)) and
// the rest after them.
struct ChunkSlice {
    long long from, to;
    const PO* after;  // what follows the slice, unrealised; nil when nothing
};
ChunkSlice sliceOf(proto::ProtoContext* ctx, const PO* chunk, const ChunkView& cv) {
    ChunkSlice sl{cv.off, std::min(cv.end, cv.off + kChunk), PROTO_NONE};
    (void)chunk;
    if (sl.to < cv.end) sl.after = newChunk(ctx, cv.items, sl.to, cv.end, cv.more, true);
    else sl.after = cv.more;
    return sl;
}

const PO* stepMap1(proto::ProtoContext* ctx, const PO*, const StepOperands& o) {
    Scope s(ctx, 4);
    s.set(0, seqOf(s.ctx(), o.b));
    const PO* src = s.get(0);
    if (src == PROTO_NONE) return PROTO_NONE;
    if (seqObjectKind(s.ctx(), src) == SeqObject::Chunk) {
        const ChunkView cv = chunkOf(s.ctx(), src);
        if (cv.chunked) {
            const ChunkSlice sl = sliceOf(s.ctx(), src, cv);
            s.set(1, sl.after);
            s.set(2, emptyList(s.ctx()));
            for (long long i = sl.from; i < sl.to; ++i) {
                const PO* y = call1(s.ctx(), o.a, cv.items->getAt(s.ctx(), static_cast<int>(i)));
                s.set(2, s.get(2)->asList(s.ctx())->appendLast(s.ctx(), y)->asObject(s.ctx()));
            }
            const PO* tail = s.get(1) == PROTO_NONE ? PROTO_NONE
                : lazy5(s.ctx(), StepKind::Map1, o.a, s.get(1), nullptr, nullptr, nullptr);
            s.set(3, tail);
            const proto::ProtoList* out = s.get(2)->asList(s.ctx());
            return s.ret(newChunk(s.ctx(), out, 0, static_cast<long long>(out->getSize(s.ctx())),
                                  s.get(3), true));
        }
    }
    s.set(1, call1(s.ctx(), o.a, seqFirst(s.ctx(), src)));
    s.set(2, seqMore(s.ctx(), src));
    s.set(3, lazy5(s.ctx(), StepKind::Map1, o.a, s.get(2), nullptr, nullptr, nullptr));
    return s.ret(newCons(s.ctx(), s.get(1), s.get(3)));
}

// (map f c1 c2 ...): o.a = f, o.b = list of the colls.
const PO* stepMapN(proto::ProtoContext* ctx, const PO*, const StepOperands& o) {
    const proto::ProtoList* colls = o.b->asList(ctx);
    const proto::proto_ulong n = colls->getSize(ctx);
    Scope s(ctx, 4);
    s.set(0, emptyList(s.ctx()));  // seqs, then firsts
    s.set(1, emptyList(s.ctx()));  // rests
    for (proto::proto_ulong i = 0; i < n; ++i) {
        const PO* sq = seqOf(s.ctx(), colls->getAt(s.ctx(), static_cast<int>(i)));
        if (sq == PROTO_NONE) return PROTO_NONE;
        s.set(2, sq);
        s.set(0, s.get(0)->asList(s.ctx())->appendLast(s.ctx(), seqFirst(s.ctx(), sq))->asObject(s.ctx()));
        s.set(1, s.get(1)->asList(s.ctx())->appendLast(s.ctx(), seqMore(s.ctx(), s.get(2)))->asObject(s.ctx()));
    }
    const proto::ProtoList* firsts = s.get(0)->asList(s.ctx());
    std::vector<const PO*> argv(n);
    for (proto::proto_ulong i = 0; i < n; ++i) argv[i] = firsts->getAt(s.ctx(), static_cast<int>(i));
    s.set(2, call(s.ctx(), o.a, argv.data(), static_cast<unsigned>(n)));
    s.set(3, lazy5(s.ctx(), StepKind::MapN, o.a, s.get(1), nullptr, nullptr, nullptr));
    return s.ret(newCons(s.ctx(), s.get(2), s.get(3)));
}

// (filter pred coll) and, with o.c true, (remove pred coll).
const PO* stepFilter(proto::ProtoContext* ctx, const PO*, const StepOperands& o) {
    const bool negate = o.c == PROTO_TRUE;
    Scope s(ctx, 4);
    s.set(0, seqOf(s.ctx(), o.b));
    for (;;) {
        const PO* src = s.get(0);
        if (src == PROTO_NONE) return PROTO_NONE;
        if (seqObjectKind(s.ctx(), src) == SeqObject::Chunk) {
            const ChunkView cv = chunkOf(s.ctx(), src);
            if (cv.chunked) {
                const ChunkSlice sl = sliceOf(s.ctx(), src, cv);
                s.set(1, sl.after);
                s.set(2, emptyList(s.ctx()));
                for (long long i = sl.from; i < sl.to; ++i) {
                    const PO* x = cv.items->getAt(s.ctx(), static_cast<int>(i));
                    if (truthy(call1(s.ctx(), o.a, x)) != negate)
                        s.set(2, s.get(2)->asList(s.ctx())->appendLast(s.ctx(), x)->asObject(s.ctx()));
                }
                const proto::ProtoList* out = s.get(2)->asList(s.ctx());
                const long long k = static_cast<long long>(out->getSize(s.ctx()));
                if (k == 0) {
                    s.set(0, seqOf(s.ctx(), s.get(1)));
                    s.tick();
                    continue;
                }
                s.set(3, s.get(1) == PROTO_NONE ? PROTO_NONE
                    : lazy5(s.ctx(), StepKind::Filter, o.a, s.get(1), o.c, nullptr, nullptr));
                return s.ret(newChunk(s.ctx(), s.get(2)->asList(s.ctx()), 0, k, s.get(3), true));
            }
        }
        const PO* x = seqFirst(s.ctx(), src);
        s.set(1, x);
        if (truthy(call1(s.ctx(), o.a, x)) != negate) {
            s.set(2, lazy5(s.ctx(), StepKind::Filter, o.a, seqMore(s.ctx(), s.get(0)), o.c, nullptr, nullptr));
            return s.ret(newCons(s.ctx(), s.get(1), s.get(2)));
        }
        s.set(0, seqNext(s.ctx(), s.get(0)));
        s.tick();
    }
}

long long countOperand(proto::ProtoContext* ctx, const PO* n) {
    if (n->isInteger(ctx)) return n->asLong(ctx);
    if (n->isFloat(ctx)) return static_cast<long long>(std::ceil(n->asDouble(ctx)));
    throwNotANumber(ctx, "take", n);
}

const PO* stepTake(proto::ProtoContext* ctx, const PO*, const StepOperands& o) {
    const long long n = countOperand(ctx, o.a);
    if (n <= 0) return PROTO_NONE;
    Scope s(ctx, 3);
    s.set(0, seqOf(s.ctx(), o.b));
    if (s.get(0) == PROTO_NONE) return PROTO_NONE;
    if (n == 1) return s.ret(newCons(s.ctx(), seqFirst(s.ctx(), s.get(0)), PROTO_NONE));
    s.set(1, seqMore(s.ctx(), s.get(0)));
    s.set(2, lazy5(s.ctx(), StepKind::Take, s.ctx()->fromLong(n - 1), s.get(1), nullptr, nullptr, nullptr));
    return s.ret(newCons(s.ctx(), seqFirst(s.ctx(), s.get(0)), s.get(2)));
}

const PO* stepTakeWhile(proto::ProtoContext* ctx, const PO*, const StepOperands& o) {
    Scope s(ctx, 3);
    s.set(0, seqOf(s.ctx(), o.b));
    if (s.get(0) == PROTO_NONE) return PROTO_NONE;
    const PO* x = seqFirst(s.ctx(), s.get(0));
    s.set(1, x);
    if (!truthy(call1(s.ctx(), o.a, x))) return PROTO_NONE;
    s.set(2, lazy5(s.ctx(), StepKind::TakeWhile, o.a, seqMore(s.ctx(), s.get(0)), nullptr, nullptr, nullptr));
    return s.ret(newCons(s.ctx(), s.get(1), s.get(2)));
}

const PO* stepDrop(proto::ProtoContext* ctx, const PO*, const StepOperands& o) {
    long long n = countOperand(ctx, o.a);
    Scope s(ctx, 1);
    SeqWalk w(s.ctx(), 0, s.ctx(), o.b);
    for (; n > 0 && !w.done(); --n) w.advance(s.ctx());
    return s.ret(w.done() ? PROTO_NONE : seqOf(s.ctx(), w.remaining(s.ctx())));
}

const PO* stepDropWhile(proto::ProtoContext* ctx, const PO*, const StepOperands& o) {
    Scope s(ctx, 1);
    SeqWalk w(s.ctx(), 0, s.ctx(), o.b);
    while (!w.done() && truthy(call1(s.ctx(), o.a, w.first(s.ctx())))) {
        w.advance(s.ctx());
        s.tick();
    }
    return s.ret(w.done() ? PROTO_NONE : seqOf(s.ctx(), w.remaining(s.ctx())));
}

// (iterate f x): x, then (f x), (f (f x)), ...; f is first called when the
// second element is read.
const PO* stepIterate(proto::ProtoContext* ctx, const PO*, const StepOperands& o) {
    Scope s(ctx, 1);
    s.set(0, lazy5(s.ctx(), StepKind::IterateNext, o.a, o.b, nullptr, nullptr, nullptr));
    return s.ret(newCons(s.ctx(), o.b, s.get(0)));
}

const PO* stepIterateNext(proto::ProtoContext* ctx, const PO*, const StepOperands& o) {
    Scope s(ctx, 2);
    s.set(0, call1(s.ctx(), o.a, o.b));
    s.set(1, lazy5(s.ctx(), StepKind::IterateNext, o.a, s.get(0), nullptr, nullptr, nullptr));
    return s.ret(newCons(s.ctx(), s.get(0), s.get(1)));
}

// (repeat x): one cons whose more is the lazy seq itself, so an endless
// repeat takes constant memory; (repeat n x) counts down.
const PO* stepRepeat(proto::ProtoContext* ctx, const PO* self, const StepOperands& o) {
    if (o.b == PROTO_NONE) return newCons(ctx, o.a, self);
    const long long n = countOperand(ctx, o.b);
    if (n <= 0) return PROTO_NONE;
    Scope s(ctx, 1);
    s.set(0, n == 1 ? PROTO_NONE
        : lazy5(s.ctx(), StepKind::Repeat, o.a, s.ctx()->fromLong(n - 1), nullptr, nullptr, nullptr));
    return s.ret(newCons(s.ctx(), o.a, s.get(0)));
}

// (repeatedly f) / (repeatedly n f): o.a = f, o.b = n or nil.
const PO* stepRepeatedly(proto::ProtoContext* ctx, const PO*, const StepOperands& o) {
    long long n = -1;
    if (o.b != PROTO_NONE) {
        n = countOperand(ctx, o.b);
        if (n <= 0) return PROTO_NONE;
    }
    Scope s(ctx, 2);
    s.set(0, call(s.ctx(), o.a, nullptr, 0));
    s.set(1, n == 1 ? PROTO_NONE
        : lazy5(s.ctx(), StepKind::Repeatedly, o.a,
                n < 0 ? PROTO_NONE : s.ctx()->fromLong(n - 1), nullptr, nullptr, nullptr));
    return s.ret(newCons(s.ctx(), s.get(0), s.get(1)));
}

// (cycle coll): o.a = the collection, o.b = where this turn is.
const PO* stepCycle(proto::ProtoContext* ctx, const PO*, const StepOperands& o) {
    Scope s(ctx, 3);
    s.set(0, seqOf(s.ctx(), o.b));
    if (s.get(0) == PROTO_NONE) s.set(0, seqOf(s.ctx(), o.a));
    if (s.get(0) == PROTO_NONE) return PROTO_NONE;
    s.set(1, seqMore(s.ctx(), s.get(0)));
    s.set(2, lazy5(s.ctx(), StepKind::Cycle, o.a, s.get(1), nullptr, nullptr, nullptr));
    return s.ret(newCons(s.ctx(), seqFirst(s.ctx(), s.get(0)), s.get(2)));
}

bool smallInts(const PO* a, const PO* b, const PO* c) {
    return proto::isSmallInt(a) && proto::isSmallInt(b) && proto::isSmallInt(c);
}

// (range start end step), finite: chunks of 32, as Clojure's LongRange.
// o.a = the next number, o.b = end, o.c = step (never 0 here).
const PO* stepRange(proto::ProtoContext* ctx, const PO*, const StepOperands& o) {
    Scope s(ctx, 3);
    const bool up = numberCompare(s.ctx(), o.c, s.ctx()->fromLong(0)) > 0;
    s.set(0, o.a);
    s.set(1, emptyList(s.ctx()));
    long long k = 0;
    if (smallInts(o.a, o.b, o.c)) {
        long long x = proto::asSmallInt(o.a);
        const long long end = proto::asSmallInt(o.b), step = proto::asSmallInt(o.c);
        const proto::ProtoList* out = s.get(1)->asList(s.ctx());
        for (; k < kChunk && (up ? x < end : x > end); ++k) {
            out = out->appendLast(s.ctx(), proto::makeSmallInt(x));
            if (!proto::smallIntInRange(x + step)) { ++k; x += step; break; }
            x += step;
        }
        s.set(1, out->asObject(s.ctx()));
        s.set(0, s.ctx()->fromLong(x));
    } else {
        for (; k < kChunk; ++k) {
            const int c = numberCompare(s.ctx(), s.get(0), o.b);
            if (up ? c >= 0 : c <= 0) break;
            s.set(1, s.get(1)->asList(s.ctx())->appendLast(s.ctx(), s.get(0))->asObject(s.ctx()));
            s.set(0, numberAdd(s.ctx(), s.get(0), o.c));
        }
    }
    if (k == 0) return PROTO_NONE;
    const int c = numberCompare(s.ctx(), s.get(0), o.b);
    const bool more = up ? c < 0 : c > 0;
    s.set(2, more ? lazy5(s.ctx(), StepKind::Range, s.get(0), o.b, o.c, nullptr, nullptr) : PROTO_NONE);
    return s.ret(newChunk(s.ctx(), s.get(1)->asList(s.ctx()), 0, k, s.get(2), true));
}

// (range): 0, 1, 2, ... without end, one at a time (Clojure's (iterate inc'
// 0)); o.a = the next number, o.b = step.
const PO* stepRangeInf(proto::ProtoContext* ctx, const PO*, const StepOperands& o) {
    Scope s(ctx, 2);
    s.set(0, numberAdd(s.ctx(), o.a, o.b));
    s.set(1, lazy5(s.ctx(), StepKind::RangeInf, s.get(0), o.b, nullptr, nullptr, nullptr));
    return s.ret(newCons(s.ctx(), o.a, s.get(1)));
}

// (concat ...) and mapcat: o.a = the collection being walked (nil before
// the first), o.b = a seqable of the collections after it.
const PO* stepConcat(proto::ProtoContext* ctx, const PO*, const StepOperands& o) {
    Scope s(ctx, 3);
    s.set(0, seqOf(s.ctx(), o.a));
    s.set(1, o.b);
    while (s.get(0) == PROTO_NONE) {
        s.set(2, seqOf(s.ctx(), s.get(1)));
        if (s.get(2) == PROTO_NONE) return PROTO_NONE;
        s.set(1, seqMore(s.ctx(), s.get(2)));
        s.set(0, seqOf(s.ctx(), seqFirst(s.ctx(), s.get(2))));
        s.tick();
    }
    return s.ret(prependSeq(s.ctx(), s.get(0), s.get(1)));
}

const PO* prependSeq(proto::ProtoContext* ctx, const PO* sq, const PO* restColls) {
    // Nothing after: the seq itself.
    if (restColls == PROTO_NONE || (isListTag(restColls) && restColls->asList(ctx)->getSize(ctx) == 0))
        return sq;
    Scope s(ctx, 2);
    if (isListTag(sq)) {
        s.set(0, lazy5(s.ctx(), StepKind::Concat, PROTO_NONE, restColls, nullptr, nullptr, nullptr));
        const proto::ProtoList* l = sq->asList(s.ctx());
        return s.ret(newChunk(s.ctx(), l, 0, static_cast<long long>(l->getSize(s.ctx())), s.get(0), false));
    }
    if (seqObjectKind(s.ctx(), sq) == SeqObject::Chunk) {
        const ChunkView cv = chunkOf(s.ctx(), sq);
        s.set(0, lazy5(s.ctx(), StepKind::Concat, cv.more, restColls, nullptr, nullptr, nullptr));
        return s.ret(newChunk(s.ctx(), cv.items, cv.off, cv.end, s.get(0), cv.chunked));
    }
    s.set(0, lazy5(s.ctx(), StepKind::Concat, consMore(s.ctx(), sq), restColls,
                   nullptr, nullptr, nullptr));
    return s.ret(newCons(s.ctx(), consFirst(s.ctx(), sq), s.get(0)));
}

// partition / partition-all: o.a = n, o.b = step, o.c = pad (mode 1), o.d =
// mode (0 partition, 1 partition with pad, 2 partition-all), o.e = coll.
const PO* stepPartition(proto::ProtoContext* ctx, const PO*, const StepOperands& o) {
    const long long n = countOperand(ctx, o.a);
    const long long step = countOperand(ctx, o.b);
    const long long mode = longOf(ctx, o.d);
    Scope s(ctx, 4);
    s.set(0, seqOf(s.ctx(), o.e));
    if (s.get(0) == PROTO_NONE) return PROTO_NONE;
    s.set(1, emptyList(s.ctx()));
    long long k = 0;
    {
        SeqWalk w(s.ctx(), 2, s.ctx(), s.get(0));
        for (; k < n && !w.done(); ++k) {
            s.set(1, s.get(1)->asList(s.ctx())->appendLast(s.ctx(), w.first(s.ctx()))->asObject(s.ctx()));
            w.advance(s.ctx());
        }
    }
    if (k < n) {
        if (mode == 0 || (mode == 2 && k == 0)) return PROTO_NONE;
        if (mode == 1) {
            SeqWalk w(s.ctx(), 2, s.ctx(), o.c);
            for (; k < n && !w.done(); ++k) {
                s.set(1, s.get(1)->asList(s.ctx())->appendLast(s.ctx(), w.first(s.ctx()))->asObject(s.ctx()));
                w.advance(s.ctx());
            }
        }
        return s.ret(newCons(s.ctx(), s.get(1), PROTO_NONE));
    }
    // The next partition starts `step` elements on.
    {
        SeqWalk w(s.ctx(), 2, s.ctx(), s.get(0));
        for (long long i = 0; i < step && !w.done(); ++i) w.advance(s.ctx());
        s.set(3, w.done() ? PROTO_NONE : w.remaining(s.ctx()));
    }
    s.set(2, s.get(3) == PROTO_NONE ? PROTO_NONE
        : lazy5(s.ctx(), StepKind::Partition, o.a, o.b, o.c, o.d, s.get(3)));
    return s.ret(newCons(s.ctx(), s.get(1), s.get(2)));
}

// (interleave c1 c2 ...): o.a = the list of colls.
const PO* stepInterleave(proto::ProtoContext* ctx, const PO*, const StepOperands& o) {
    const proto::ProtoList* colls = o.a->asList(ctx);
    const proto::proto_ulong n = colls->getSize(ctx);
    if (n == 0) return PROTO_NONE;
    Scope s(ctx, 4);
    s.set(0, emptyList(s.ctx()));  // firsts
    s.set(1, emptyList(s.ctx()));  // rests
    for (proto::proto_ulong i = 0; i < n; ++i) {
        s.set(2, seqOf(s.ctx(), colls->getAt(s.ctx(), static_cast<int>(i))));
        if (s.get(2) == PROTO_NONE) return PROTO_NONE;
        s.set(0, s.get(0)->asList(s.ctx())->appendLast(s.ctx(), seqFirst(s.ctx(), s.get(2)))->asObject(s.ctx()));
        s.set(1, s.get(1)->asList(s.ctx())->appendLast(s.ctx(), seqMore(s.ctx(), s.get(2)))->asObject(s.ctx()));
    }
    s.set(3, lazy5(s.ctx(), StepKind::Interleave, s.get(1), nullptr, nullptr, nullptr, nullptr));
    const proto::ProtoList* firsts = s.get(0)->asList(s.ctx());
    return s.ret(newChunk(s.ctx(), firsts, 0, static_cast<long long>(n), s.get(3), false));
}

// for (core.clj, M27): o.a = the collection, o.b = the body fn of one
// binding, o.c = the stop marker (:while false), o.d = true for the
// innermost binding, whose body answers a list of at most one element; it is
// the one that walks a chunked collection a chunk at a time, as Clojure's
// for does.
const PO* stepFor(proto::ProtoContext* ctx, const PO*, const StepOperands& o) {
    const bool inner = o.d == PROTO_TRUE;
    Scope s(ctx, 4);
    s.set(0, seqOf(s.ctx(), o.a));
    for (;;) {
        const PO* src = s.get(0);
        if (src == PROTO_NONE) return PROTO_NONE;
        if (inner && seqObjectKind(s.ctx(), src) == SeqObject::Chunk) {
            const ChunkView cv = chunkOf(s.ctx(), src);
            if (cv.chunked) {
                const ChunkSlice sl = sliceOf(s.ctx(), src, cv);
                s.set(1, sl.after);
                s.set(2, emptyList(s.ctx()));
                bool stopped = false;
                for (long long i = sl.from; i < sl.to; ++i) {
                    const PO* r = call1(s.ctx(), o.b, cv.items->getAt(s.ctx(), static_cast<int>(i)));
                    if (r == o.c) { stopped = true; break; }
                    s.set(3, r);
                    if (const proto::ProtoList* rl = seqToList(s.ctx(), s.get(3))) {
                        const proto::proto_ulong rn = rl->getSize(s.ctx());
                        for (proto::proto_ulong j = 0; j < rn; ++j)
                            s.set(2, s.get(2)->asList(s.ctx())
                                ->appendLast(s.ctx(), rl->getAt(s.ctx(), static_cast<int>(j)))->asObject(s.ctx()));
                    }
                }
                const proto::ProtoList* out = s.get(2)->asList(s.ctx());
                const long long k = static_cast<long long>(out->getSize(s.ctx()));
                if (stopped) s.set(1, PROTO_NONE);
                if (k == 0) {
                    if (stopped) return PROTO_NONE;
                    s.set(0, seqOf(s.ctx(), s.get(1)));
                    s.tick();
                    continue;
                }
                s.set(3, s.get(1) == PROTO_NONE ? PROTO_NONE
                    : lazy5(s.ctx(), StepKind::For, s.get(1), o.b, o.c, o.d, nullptr));
                return s.ret(newChunk(s.ctx(), s.get(2)->asList(s.ctx()), 0, k, s.get(3), true));
            }
        }
        const PO* r = call1(s.ctx(), o.b, seqFirst(s.ctx(), src));
        if (r == o.c) return PROTO_NONE;
        s.set(1, r);
        s.set(2, seqOf(s.ctx(), s.get(1)));
        if (s.get(2) != PROTO_NONE) {
            s.set(3, lazy5(s.ctx(), StepKind::For, seqMore(s.ctx(), s.get(0)), o.b, o.c, o.d, nullptr));
            s.set(1, s.ctx()->newList()->appendLast(s.ctx(), s.get(3))->asObject(s.ctx()));
            return s.ret(prependSeq(s.ctx(), s.get(2), s.get(1)));
        }
        s.set(0, seqNext(s.ctx(), s.get(0)));
        s.tick();
    }
}

// (distinct coll): o.a = the collection, o.b = the set of elements seen.
const PO* stepDistinct(proto::ProtoContext* ctx, const PO*, const StepOperands& o) {
    Scope s(ctx, 3);
    s.set(0, seqOf(s.ctx(), o.a));
    s.set(1, o.b == PROTO_NONE ? setConj(s.ctx(), nullptr, nullptr, 0) : o.b);
    for (;;) {
        if (s.get(0) == PROTO_NONE) return PROTO_NONE;
        const PO* x = seqFirst(s.ctx(), s.get(0));
        bool seen = false;
        setGet(s.ctx(), s.get(1), x, &seen);
        if (!seen) {
            s.set(2, x);
            s.set(1, setConj(s.ctx(), s.get(1), &x, 1));
            s.set(1, lazy5(s.ctx(), StepKind::Distinct, seqMore(s.ctx(), s.get(0)), s.get(1),
                           nullptr, nullptr, nullptr));
            return s.ret(newCons(s.ctx(), s.get(2), s.get(1)));
        }
        s.set(0, seqNext(s.ctx(), s.get(0)));
        s.tick();
    }
}

const proto::ProtoList* seqToListHook(proto::ProtoContext* ctx, const PO* v) {
    return seqToList(ctx, v);
}

} // namespace

// ---- construction -------------------------------------------------------------

void initLazySeqs(proto::ProtoContext* ctx) {
    (void)ctx;
    setSeqToList(&seqToListHook);
    registerStep(StepKind::Map1, &stepMap1, false);
    registerStep(StepKind::MapN, &stepMapN, false);
    registerStep(StepKind::Filter, &stepFilter, false);
    registerStep(StepKind::Take, &stepTake, true);
    registerStep(StepKind::TakeWhile, &stepTakeWhile, false);
    registerStep(StepKind::Drop, &stepDrop, true);
    registerStep(StepKind::DropWhile, &stepDropWhile, false);
    registerStep(StepKind::Iterate, &stepIterate, true);
    registerStep(StepKind::IterateNext, &stepIterateNext, false);
    registerStep(StepKind::Repeat, &stepRepeat, true);
    registerStep(StepKind::Repeatedly, &stepRepeatedly, false);
    registerStep(StepKind::Cycle, &stepCycle, true);
    registerStep(StepKind::Range, &stepRange, true);
    registerStep(StepKind::RangeInf, &stepRangeInf, true);
    registerStep(StepKind::Concat, &stepConcat, true);
    registerStep(StepKind::Partition, &stepPartition, true);
    registerStep(StepKind::Interleave, &stepInterleave, true);
    registerStep(StepKind::For, &stepFor, false);
    registerStep(StepKind::Distinct, &stepDistinct, true);
}

void registerStep(StepKind kind, StepRealizer fn, bool pure) {
    g_steps[static_cast<int>(kind)] = StepEntry{fn, pure};
}

// A lazy seq is a protoCore lazy object (ProtoObject::newLazyChild, protoCore
// 3.1): an immutable child of the lazy seq prototype whose lazy slot holds
// the thunk as its producer and, once realised, the seq. No mutable object
// is involved, so a realised chain is not rooted by the global mutable table
// (M28).
const PO* newLazySeq(proto::ProtoContext* ctx, const PO* thunk) {
    return g_seqProtos.lazy->newLazyChild(ctx, orNil(thunk));
}

const PO* newCons(proto::ProtoContext* ctx, const PO* first, const PO* more) {
    const PO* vals[2] = {orNil(first), orNil(more)};
    return newNode(ctx, g_seqProtos.cons, 2, vals);
}

const PO* newChunk(proto::ProtoContext* ctx, const proto::ProtoList* items, long long off,
                   long long end, const PO* more, bool chunked) {
    const PO* vals[4] = {items->asObject(ctx), proto::makeSmallInt(off),
                         proto::makeSmallInt(chunked ? -end - 1 : end), orNil(more)};
    return newNode(ctx, g_seqProtos.chunk, 4, vals);
}

const PO* lazyStep(proto::ProtoContext* ctx, StepKind kind, const PO* a, const PO* b,
                   const PO* c, const PO* d) {
    return lazy5(ctx, kind, a, b, c, d, nullptr);
}

const PO* lazyStep5(proto::ProtoContext* ctx, StepKind kind, const PO* a, const PO* b,
                    const PO* c, const PO* d, const PO* e) {
    return lazy5(ctx, kind, a, b, c, d, e);
}

// ---- realisation --------------------------------------------------------------

const PO* realizeLazy(proto::ProtoContext* ctx, const PO* ls) {
    checkNativeStack();
    for (unsigned round = 0;; ++round) {
        switch (ls->getLazyState(ctx)) {
            case proto::ProtoObject::LazyRealized:
                return orNil(ls->getLazyValue(ctx));
            case proto::ProtoObject::LazyClaimed:
            case proto::ProtoObject::LazyPublishing:
                if (std::find(tl_realizing.begin(), tl_realizing.end(), ls) != tl_realizing.end())
                    throwClassed(ctx, "StackOverflowError",
                                 "a lazy seq was read while its own body was computing it");
                waitABit(ctx, round);
                continue;
            case proto::ProtoObject::LazyUnrealized:
                break;
            default:
                throwError(ctx, "lazy seq: not a lazy object");
        }
        // The thunk, rooted while it runs (the object stops tracing it once
        // the seq is published).
        Scope s(ctx, 2);
        const PO* thunk = ls->getLazyProducer(s.ctx());
        if (!thunk) continue;  // published meanwhile
        s.set(0, thunk);
        if (thunkIsPure(s.ctx(), thunk)) {
            // No Clojure code runs: computing it twice in a race is harmless;
            // the first publication wins and the loser reads it.
            s.set(1, runThunk(s.ctx(), ls, thunk));
            if (ls->publishLazy(s.ctx(), s.get(1))) return s.get(1);
            continue;
        }
        if (!ls->claimLazy(s.ctx())) continue;
        tl_realizing.push_back(ls);
        try {
            s.set(1, runThunk(s.ctx(), ls, s.get(0)));
        } catch (...) {
            tl_realizing.pop_back();
            // Unrealised again, as Clojure leaves a LazySeq whose fn raised.
            ls->unclaimLazy(s.ctx());
            throw;
        }
        tl_realizing.pop_back();
        if (ls->publishLazy(s.ctx(), s.get(1))) return s.get(1);
        // Published by someone else meanwhile: every reader sees one value.
        return orNil(ls->getLazyValue(s.ctx()));
    }
}

bool lazyIsRealized(proto::ProtoContext* ctx, const PO* ls) {
    return ls->getLazyState(ctx) == proto::ProtoObject::LazyRealized;
}

bool isChunkedSeq(proto::ProtoContext* ctx, const PO* s) {
    if (seqObjectKind(ctx, s) != SeqObject::Chunk) return false;
    return proto::asSmallInt(fieldsOf(ctx, s)->getAt(ctx, 2)) < 0;
}

// ---- the seq protocol ---------------------------------------------------------

const PO* seqOf(proto::ProtoContext* ctx, const PO* v) {
    if (!v || v == PROTO_NONE) return PROTO_NONE;
    if (isListTag(v)) return v->asList(ctx)->getSize(ctx) == 0 ? PROTO_NONE : v;
    if (isVector(v)) {
        const proto::ProtoList* items = vectorItems(ctx, v);
        const long long n = static_cast<long long>(items->getSize(ctx));
        return n == 0 ? PROTO_NONE : newChunk(ctx, items, 0, n, PROTO_NONE, true);
    }
    switch (seqObjectKind(ctx, v)) {
        case SeqObject::Lazy:  return realizeLazy(ctx, v);
        case SeqObject::Cons:
        case SeqObject::Chunk: return v;
        case SeqObject::None:  break;
    }
    const proto::ProtoList* items = nullptr;
    if (collectionItems(ctx, v, &items))
        return (items && items->getSize(ctx) > 0) ? items->asObject(ctx) : PROTO_NONE;
    throwClassed(ctx, "IllegalArgumentException",
                 "Don't know how to create ISeq from: " + std::string(valueTypeName(ctx, v)));
}

const PO* seqFirst(proto::ProtoContext* ctx, const PO* s) {
    if (isListTag(s)) return s->asList(ctx)->getFirst(ctx);
    if (seqObjectKind(ctx, s) == SeqObject::Cons) return consFirst(ctx, s);
    const ChunkView cv = chunkOf(ctx, s);
    return cv.items->getAt(ctx, static_cast<int>(cv.off));
}

const PO* seqMore(proto::ProtoContext* ctx, const PO* s) {
    if (isListTag(s)) {
        const proto::ProtoList* l = s->asList(ctx);
        return l->getSize(ctx) <= 1 ? emptyList(ctx) : l->removeFirst(ctx)->asObject(ctx);
    }
    if (seqObjectKind(ctx, s) == SeqObject::Cons) {
        const PO* m = consMore(ctx, s);
        return m == PROTO_NONE ? emptyList(ctx) : m;
    }
    const ChunkView cv = chunkOf(ctx, s);
    if (cv.off + 1 < cv.end) return newChunk(ctx, cv.items, cv.off + 1, cv.end, cv.more, cv.chunked);
    return cv.more == PROTO_NONE ? emptyList(ctx) : cv.more;
}

bool isSeqValue(proto::ProtoContext* ctx, const PO* v) {
    return isListTag(v) || isSeqObject(ctx, v);
}

const proto::ProtoList* seqToList(proto::ProtoContext* ctx, const PO* v) {
    if (!v || v == PROTO_NONE) return nullptr;
    if (isListTag(v)) {
        const proto::ProtoList* l = v->asList(ctx);
        return l->getSize(ctx) == 0 ? nullptr : l;
    }
    if (isVector(v)) {
        const proto::ProtoList* l = vectorItems(ctx, v);
        return l->getSize(ctx) == 0 ? nullptr : l;
    }
    if (!isSeqObject(ctx, v)) {
        const proto::ProtoList* items = nullptr;
        if (collectionItems(ctx, v, &items))
            return (items && items->getSize(ctx) > 0) ? items : nullptr;
        throwClassed(ctx, "IllegalArgumentException",
                     "Don't know how to create ISeq from: " + std::string(valueTypeName(ctx, v)));
    }
    Scope s(ctx, 2);
    s.set(1, emptyList(s.ctx()));
    {
        SeqWalk w(s.ctx(), 0, s.ctx(), v);
        // A list is built by appending, which leaves a path of garbage per
        // element: the build runs in chunks, each in a context of its own.
        while (!w.done()) {
            proto::ProtoContext chunk(s.ctx()->space, s.ctx());
            const proto::ProtoList* acc = s.get(1)->asList(&chunk);
            for (int i = 0; i < 256 && !w.done(); ++i) {
                acc = acc->appendLast(&chunk, w.first(&chunk));
                s.set(1, acc->asObject(&chunk));
                w.advance(&chunk);
            }
        }
    }
    const proto::ProtoList* out = s.get(1)->asList(s.ctx());
    if (out->getSize(s.ctx()) == 0) return nullptr;
    s.ret(out->asObject(s.ctx()));
    return out;
}

// ---- walking --------------------------------------------------------------------

SeqWalk::SeqWalk(proto::ProtoContext* root, unsigned slot, proto::ProtoContext* ctx,
                 const PO* coll)
    : root_(root), slot_(slot) {
    proto::ProtoContext step(ctx->space, ctx);
    const PO* s = seqOf(&step, coll);
    root_->setAutomaticLocal(slot_, s);
    load(&step, s);
}

void SeqWalk::load(proto::ProtoContext* ctx, const PO* node) {
    items_ = nullptr;
    cons_ = false;
    if (!node || node == PROTO_NONE) { node_ = nullptr; return; }
    node_ = node;
    if (isListTag(node)) {
        items_ = node->asList(ctx);
        i_ = 0;
        end_ = static_cast<long long>(items_->getSize(ctx));
        return;
    }
    if (seqObjectKind(ctx, node) == SeqObject::Cons) { cons_ = true; return; }
    const ChunkView cv = chunkOf(ctx, node);
    items_ = cv.items;
    i_ = cv.off;
    end_ = cv.end;
}

const PO* SeqWalk::first(proto::ProtoContext* ctx) const {
    if (cons_) return consFirst(ctx, node_);
    return items_->getAt(ctx, static_cast<int>(i_));
}

void SeqWalk::advance(proto::ProtoContext* ctx) {
    if (!node_) return;
    if (!cons_ && ++i_ < end_) return;
    const PO* more = isListTag(node_) ? PROTO_NONE
        : (cons_ ? consMore(ctx, node_) : chunkOf(ctx, node_).more);
    if (more == PROTO_NONE) {
        root_->setAutomaticLocal(slot_, PROTO_NONE);
        node_ = nullptr;
        items_ = nullptr;
        return;
    }
    proto::ProtoContext step(ctx->space, ctx);
    const PO* s = seqOf(&step, more);
    root_->setAutomaticLocal(slot_, s);
    load(&step, s);
}

const PO* SeqWalk::remaining(proto::ProtoContext* ctx) const {
    if (!node_) return PROTO_NONE;
    if (cons_) return node_;
    if (isListTag(node_)) {
        if (i_ == 0) return node_;
        return newChunk(ctx, items_, i_, end_, PROTO_NONE, false);
    }
    const ChunkView cv = chunkOf(ctx, node_);
    if (i_ == cv.off) return node_;
    return newChunk(ctx, items_, i_, end_, cv.more, cv.chunked);
}

void releaseArg(const proto::ProtoArguments* args, unsigned i) {
    if (!args || i >= args->count()) return;
    const_cast<const PO**>(args->values())[i] = nullptr;
}

// ---- primitives ------------------------------------------------------------------

namespace {

#define LAZYPRIM(name) \
    const PO* name(proto::ProtoContext* ctx, const PO*, const proto::ParentLink*, \
                   const proto::ProtoArguments* args, const proto::ProtoKeywordArguments*)

proto::proto_ulong argc(proto::ProtoContext* ctx, const proto::ProtoArguments* args) {
    return args ? args->getSize(ctx) : 0;
}
const PO* argAt(proto::ProtoContext* ctx, const proto::ProtoArguments* args, proto::proto_ulong i) {
    return orNil(args->getAt(ctx, static_cast<int>(i)));
}
[[noreturn]] void arity(proto::ProtoContext* ctx, proto::proto_ulong n, const char* name) {
    throwClassed(ctx, "ArityException",
                 "Wrong number of args (" + std::to_string(n) + ") passed to: " + name);
}
void requireNumber(proto::ProtoContext* ctx, const PO* v, const char* name) {
    if (!isNumber(ctx, v)) throwNotANumber(ctx, name, v);
}

// The arguments from index `from` on, as a list (rooted by the caller's
// scope while it is used).
const PO* argsFrom(proto::ProtoContext* ctx, const proto::ProtoArguments* args, proto::proto_ulong from) {
    const proto::ProtoList* l = ctx->newList();
    const proto::proto_ulong n = argc(ctx, args);
    proto::ProtoContext::CriticalSection guard(ctx);
    for (proto::proto_ulong i = from; i < n; ++i) l = l->appendLast(ctx, argAt(ctx, args, i));
    return l->asObject(ctx);
}

LAZYPRIM(prim_lazy_seq_fn) {
    if (argc(ctx, args) != 1) arity(ctx, argc(ctx, args), "lazy-seq");
    // A list producer is a native step; the lazy-seq macro passes a fn.
    if (isListTag(argAt(ctx, args, 0)))
        throwClassed(ctx, "ClassCastException", "__lazy-seq expects a fn of no arguments");
    return newLazySeq(ctx, argAt(ctx, args, 0));
}

LAZYPRIM(prim_map) {
    const proto::proto_ulong n = argc(ctx, args);
    if (n < 2) {
        if (n == 1)
            throwClassed(ctx, "UnsupportedOperationException",
                         "map: the transducer arity (map f) is not supported (D49)");
        arity(ctx, n, "map");
    }
    if (n == 2) return lazyStep(ctx, StepKind::Map1, argAt(ctx, args, 0), argAt(ctx, args, 1));
    Scope s(ctx, 1);
    s.set(0, argsFrom(s.ctx(), args, 1));
    return s.ret(lazyStep(s.ctx(), StepKind::MapN, argAt(ctx, args, 0), s.get(0)));
}

LAZYPRIM(prim_filter) {
    if (argc(ctx, args) != 2) arity(ctx, argc(ctx, args), "filter");
    return lazyStep(ctx, StepKind::Filter, argAt(ctx, args, 0), argAt(ctx, args, 1), PROTO_FALSE);
}

LAZYPRIM(prim_remove) {
    if (argc(ctx, args) != 2) arity(ctx, argc(ctx, args), "remove");
    return lazyStep(ctx, StepKind::Filter, argAt(ctx, args, 0), argAt(ctx, args, 1), PROTO_TRUE);
}

LAZYPRIM(prim_take) {
    if (argc(ctx, args) != 2) arity(ctx, argc(ctx, args), "take");
    requireNumber(ctx, argAt(ctx, args, 0), "take");
    return lazyStep(ctx, StepKind::Take, argAt(ctx, args, 0), argAt(ctx, args, 1));
}

LAZYPRIM(prim_take_while) {
    if (argc(ctx, args) != 2) arity(ctx, argc(ctx, args), "take-while");
    return lazyStep(ctx, StepKind::TakeWhile, argAt(ctx, args, 0), argAt(ctx, args, 1));
}

LAZYPRIM(prim_drop) {
    if (argc(ctx, args) != 2) arity(ctx, argc(ctx, args), "drop");
    requireNumber(ctx, argAt(ctx, args, 0), "drop");
    return lazyStep(ctx, StepKind::Drop, argAt(ctx, args, 0), argAt(ctx, args, 1));
}

LAZYPRIM(prim_drop_while) {
    if (argc(ctx, args) != 2) arity(ctx, argc(ctx, args), "drop-while");
    return lazyStep(ctx, StepKind::DropWhile, argAt(ctx, args, 0), argAt(ctx, args, 1));
}

LAZYPRIM(prim_iterate) {
    if (argc(ctx, args) != 2) arity(ctx, argc(ctx, args), "iterate");
    return lazyStep(ctx, StepKind::Iterate, argAt(ctx, args, 0), argAt(ctx, args, 1));
}

LAZYPRIM(prim_repeat) {
    const proto::proto_ulong n = argc(ctx, args);
    if (n == 1) return lazyStep(ctx, StepKind::Repeat, argAt(ctx, args, 0), PROTO_NONE);
    if (n == 2) {
        requireNumber(ctx, argAt(ctx, args, 0), "repeat");
        return lazyStep(ctx, StepKind::Repeat, argAt(ctx, args, 1), argAt(ctx, args, 0));
    }
    arity(ctx, n, "repeat");
}

LAZYPRIM(prim_repeatedly) {
    const proto::proto_ulong n = argc(ctx, args);
    if (n == 1) return lazyStep(ctx, StepKind::Repeatedly, argAt(ctx, args, 0), PROTO_NONE);
    if (n == 2) {
        requireNumber(ctx, argAt(ctx, args, 0), "repeatedly");
        return lazyStep(ctx, StepKind::Repeatedly, argAt(ctx, args, 1), argAt(ctx, args, 0));
    }
    arity(ctx, n, "repeatedly");
}

LAZYPRIM(prim_cycle) {
    if (argc(ctx, args) != 1) arity(ctx, argc(ctx, args), "cycle");
    return lazyStep(ctx, StepKind::Cycle, argAt(ctx, args, 0), PROTO_NONE);
}

// (range), (range end), (range start end), (range start end step): lazy,
// in chunks of 32 when finite, as Clojure's LongRange; (range) counts up
// without end, one at a time. A zero step is an endless repeat of start
// unless start equals end, as in Clojure.
LAZYPRIM(prim_range) {
    const proto::proto_ulong n = argc(ctx, args);
    if (n > 3) arity(ctx, n, "range");
    if (n == 0) return lazyStep(ctx, StepKind::RangeInf, ctx->fromLong(0), ctx->fromLong(1));
    const PO* start = n == 1 ? ctx->fromLong(0) : argAt(ctx, args, 0);
    const PO* end   = n == 1 ? argAt(ctx, args, 0) : argAt(ctx, args, 1);
    const PO* step  = n == 3 ? argAt(ctx, args, 2) : ctx->fromLong(1);
    requireNumber(ctx, start, "range");
    requireNumber(ctx, end, "range");
    requireNumber(ctx, step, "range");
    if (numberCompare(ctx, step, ctx->fromLong(0)) == 0) {
        if (numberCompare(ctx, start, end) == 0) return ctx->newList()->asObject(ctx);
        return lazyStep(ctx, StepKind::Repeat, start, PROTO_NONE);
    }
    return lazyStep(ctx, StepKind::Range, start, end, step);
}

// (mapcat f & colls): (apply concat (map f colls)), lazily.
LAZYPRIM(prim_mapcat) {
    const proto::proto_ulong n = argc(ctx, args);
    if (n < 2) {
        if (n == 1)
            throwClassed(ctx, "UnsupportedOperationException",
                         "mapcat: the transducer arity (mapcat f) is not supported (D49)");
        arity(ctx, n, "mapcat");
    }
    Scope s(ctx, 2);
    if (n == 2) {
        s.set(0, lazyStep(s.ctx(), StepKind::Map1, argAt(ctx, args, 0), argAt(ctx, args, 1)));
    } else {
        s.set(1, argsFrom(s.ctx(), args, 1));
        s.set(0, lazyStep(s.ctx(), StepKind::MapN, argAt(ctx, args, 0), s.get(1)));
    }
    return s.ret(lazyStep(s.ctx(), StepKind::Concat, PROTO_NONE, s.get(0)));
}

const PO* partitionPrim(proto::ProtoContext* ctx, const proto::ProtoArguments* args, bool all) {
    const proto::proto_ulong n = argc(ctx, args);
    const char* name = all ? "partition-all" : "partition";
    if (n < 2 || n > (all ? 3u : 4u)) {
        if (n == 1)
            throwClassed(ctx, "UnsupportedOperationException",
                         std::string(name) + ": the transducer arity is not supported (D49)");
        arity(ctx, n, name);
    }
    const PO* size = argAt(ctx, args, 0);
    const PO* step = n >= 3 ? argAt(ctx, args, 1) : size;
    const PO* pad = n == 4 ? argAt(ctx, args, 2) : PROTO_NONE;
    const PO* coll = argAt(ctx, args, n - 1);
    requireNumber(ctx, size, name);
    requireNumber(ctx, step, name);
    const long long mode = all ? 2 : (n == 4 ? 1 : 0);
    Scope s(ctx, 1);
    s.set(0, newStep(s.ctx(), StepKind::Partition, size, step, pad, s.ctx()->fromLong(mode), coll));
    return s.ret(newLazySeq(s.ctx(), s.get(0)));
}

LAZYPRIM(prim_partition) { return partitionPrim(ctx, args, false); }
LAZYPRIM(prim_partition_all) { return partitionPrim(ctx, args, true); }

LAZYPRIM(prim_interleave) {
    const proto::proto_ulong n = argc(ctx, args);
    if (n == 0) return ctx->newList()->asObject(ctx);
    if (n == 1) return lazyStep(ctx, StepKind::Concat, argAt(ctx, args, 0), PROTO_NONE);
    Scope s(ctx, 1);
    s.set(0, argsFrom(s.ctx(), args, 0));
    return s.ret(lazyStep(s.ctx(), StepKind::Interleave, s.get(0)));
}

LAZYPRIM(prim_distinct) {
    if (argc(ctx, args) != 1) arity(ctx, argc(ctx, args), "distinct");
    return lazyStep(ctx, StepKind::Distinct, argAt(ctx, args, 0), PROTO_NONE);
}

// Walks `coll` (to `limit` elements when limit >= 0), realising it.
void walkAll(proto::ProtoContext* ctx, const PO* coll, long long limit,
             const proto::ProtoArguments* release, unsigned releaseIdx) {
    Scope s(ctx, 1);
    SeqWalk w(s.ctx(), 0, s.ctx(), coll);
    if (release) releaseArg(release, releaseIdx);
    for (long long i = 0; !w.done() && (limit < 0 || i < limit); ++i) {
        w.advance(s.ctx());
        s.tick();
    }
}

// (dorun coll) / (dorun n coll): realises the seq for its side effects and
// answers nil, holding only the walk's position, so the head may be
// collected while it runs.
LAZYPRIM(prim_dorun) {
    const proto::proto_ulong n = argc(ctx, args);
    if (n == 1) { walkAll(ctx, argAt(ctx, args, 0), -1, args, 0); return PROTO_NONE; }
    if (n == 2) {
        requireNumber(ctx, argAt(ctx, args, 0), "dorun");
        walkAll(ctx, argAt(ctx, args, 1), countOperand(ctx, argAt(ctx, args, 0)), args, 1);
        return PROTO_NONE;
    }
    arity(ctx, n, "dorun");
}

// (doall coll) / (doall n coll): realises the seq and answers it, which
// therefore holds every element.
LAZYPRIM(prim_doall) {
    const proto::proto_ulong n = argc(ctx, args);
    if (n == 1) { walkAll(ctx, argAt(ctx, args, 0), -1, nullptr, 0); return argAt(ctx, args, 0); }
    if (n == 2) {
        requireNumber(ctx, argAt(ctx, args, 0), "doall");
        walkAll(ctx, argAt(ctx, args, 1), countOperand(ctx, argAt(ctx, args, 0)), nullptr, 0);
        return argAt(ctx, args, 1);
    }
    arity(ctx, n, "doall");
}

// (run! proc coll): calls proc on each element for its side effects; nil.
LAZYPRIM(prim_run_bang) {
    if (argc(ctx, args) != 2) arity(ctx, argc(ctx, args), "run!");
    const PO* proc = argAt(ctx, args, 0);
    Scope s(ctx, 1);
    SeqWalk w(s.ctx(), 0, s.ctx(), argAt(ctx, args, 1));
    releaseArg(args, 1);
    for (; !w.done(); w.advance(s.ctx())) {
        call1(s.ctx(), proc, w.first(s.ctx()));
        s.tick();
    }
    return PROTO_NONE;
}

LAZYPRIM(prim_chunked_seq_p) {
    if (argc(ctx, args) != 1) arity(ctx, argc(ctx, args), "chunked-seq?");
    return isChunkedSeq(ctx, argAt(ctx, args, 0)) ? PROTO_TRUE : PROTO_FALSE;
}

// (__for-step coll f) / (__for-step coll f inner?) -- for's walk of one
// binding (core.clj, M27): the lazy concatenation of (f x) over coll,
// stopped by the __for-stop marker.
LAZYPRIM(prim_for_step) {
    const proto::proto_ulong n = argc(ctx, args);
    if (n != 2 && n != 3) arity(ctx, n, "__for-step");
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throwError(ctx, "__for-step: no active VM context");
    const PO* stop = orNil(cc->globals->getOwnAttributeDirect(
        ctx, proto::ProtoString::createSymbol(ctx, "clojure.core/__for-stop")));
    const PO* inner = (n == 3 && truthy(argAt(ctx, args, 2))) ? PROTO_TRUE : PROTO_FALSE;
    return lazyStep(ctx, StepKind::For, argAt(ctx, args, 0), argAt(ctx, args, 1), stop, inner);
}

#undef LAZYPRIM

struct LazyPrim { const char* name; proto::ProtoMethod fn; };
constexpr LazyPrim kLazyPrims[] = {
    {"__lazy-seq",    &prim_lazy_seq_fn},
    {"map",           &prim_map},
    {"filter",        &prim_filter},
    {"remove",        &prim_remove},
    {"take",          &prim_take},
    {"take-while",    &prim_take_while},
    {"drop",          &prim_drop},
    {"drop-while",    &prim_drop_while},
    {"iterate",       &prim_iterate},
    {"repeat",        &prim_repeat},
    {"repeatedly",    &prim_repeatedly},
    {"cycle",         &prim_cycle},
    {"range",         &prim_range},
    {"mapcat",        &prim_mapcat},
    {"partition",     &prim_partition},
    {"partition-all", &prim_partition_all},
    {"interleave",    &prim_interleave},
    {"distinct",      &prim_distinct},
    {"dorun",         &prim_dorun},
    {"doall",         &prim_doall},
    {"run!",          &prim_run_bang},
    {"chunked-seq?",  &prim_chunked_seq_p},
    {"__for-step",    &prim_for_step},
};

} // namespace

void installLazySeqPrimitives(proto::ProtoContext* ctx, proto::ProtoObject* globals) {
    for (const LazyPrim& p : kLazyPrims)
        defineBuiltin(ctx, globals, p.name, ctx->fromMethod(nullptr, p.fn));
}

const char* lazySeqPrimitiveName(proto::ProtoMethod fn) {
    for (const LazyPrim& p : kLazyPrims)
        if (p.fn == fn) return p.name;
    return nullptr;
}

} // namespace protoClojure
