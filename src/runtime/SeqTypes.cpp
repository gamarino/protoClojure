#include "CallerSlots.h"
#include "SeqTypes.h"

#include "protoCore.h"

#include <stdexcept>
#include <vector>

namespace protoClojure {

SeqPrototypes g_seqProtos;

namespace {
SeqToListFn g_seqToList = nullptr;

inline const proto::ProtoObject* orNil(const proto::ProtoObject* v) { return v ? v : PROTO_NONE; }
}

const proto::ProtoObject* initSeqTypes(proto::ProtoContext* ctx) {
    CallerSlots scope(ctx, 1);
    const proto::ProtoObject* root = ctx->space->objectPrototype;
    const proto::ProtoList* holder = ctx->newList();
    for (const proto::ProtoObject** p : {&g_seqProtos.lazy, &g_seqProtos.cons, &g_seqProtos.chunk}) {
        *p = root->newChild(ctx, /*isMutable=*/false);
        holder = holder->appendLast(ctx, *p);
        scope.set(0, holder->asObject(ctx));
    }
    // One value per process, never collected (protoCore 3.3.0).
    g_seqProtos.empty = ctx->newLinkedList()->asObject(ctx);
    return scope.get(0);
}

SeqObject seqObjectKind(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
    if (!v || !g_seqProtos.lazy) return SeqObject::None;
    const proto::proto_ulong tag = reinterpret_cast<proto::proto_ulong>(v) & 0x3F;
    if (tag == kTagLinkedList) return v == g_seqProtos.empty ? SeqObject::Empty : SeqObject::List;
    if (tag != 0) return SeqObject::None;
    const proto::ProtoObject* p = v->getPrototype(ctx);
    if (p == g_seqProtos.lazy)  return SeqObject::Lazy;
    if (p == g_seqProtos.cons)  return SeqObject::Cons;
    if (p == g_seqProtos.chunk) return SeqObject::Chunk;
    return SeqObject::None;
}

// ---- seq nodes -------------------------------------------------------------------

const proto::ProtoList* seqNodeFields(proto::ProtoContext* ctx, const proto::ProtoObject* node) {
    return node->getLazyValue(ctx)->asList(ctx);
}

const proto::ProtoObject* newSeqNode(proto::ProtoContext* ctx, const proto::ProtoObject* proto,
                                     unsigned n, const proto::ProtoObject* const* fields) {
    // A lazy object published at once with its fields: three cells for the
    // object and one for the fields list (up to five fields fit one cell).
    // Both are young cells of `ctx`, which the collector does not reclaim.
    const proto::ProtoObject* node = proto->newLazyChild(ctx, PROTO_NONE);
    node->publishLazy(ctx, ctx->newList(n, fields)->asObject(ctx));
    return node;
}

// ---- Clojure lists ----------------------------------------------------------------

namespace {
// The values at `items` with nullptr read as nil: protoCore's builders skip
// a nullptr, and a Clojure list keeps a nil element.
template <typename Fn>
const proto::ProtoObject* withNils(std::size_t n, const proto::ProtoObject* const* items, Fn build) {
    for (std::size_t i = 0; i < n; ++i) {
        if (items[i]) continue;
        std::vector<const proto::ProtoObject*> copy(items, items + n);
        for (auto& x : copy) x = orNil(x);
        return build(copy.data());
    }
    return build(items);
}
} // namespace

const proto::ProtoObject* listCons(proto::ProtoContext* ctx, const proto::ProtoObject* x,
                                   const proto::ProtoObject* rest) {
    return asLinkedList(rest)->prepend(ctx, orNil(x))->asObject(ctx);
}

const proto::ProtoObject* listFirst(proto::ProtoContext* ctx, const proto::ProtoObject* cell) {
    return orNil(asLinkedList(cell)->getFirst(ctx));
}

const proto::ProtoObject* listRest(proto::ProtoContext* ctx, const proto::ProtoObject* cell) {
    return asLinkedList(cell)->getRest(ctx)->asObject(ctx);
}

void listSplit(proto::ProtoContext* ctx, const proto::ProtoObject* cell,
               const proto::ProtoObject** first, const proto::ProtoObject** rest) {
    const proto::ProtoLinkedList* l = asLinkedList(cell);
    *first = orNil(l->getFirst(ctx));
    *rest = l->getRest(ctx)->asObject(ctx);
}

long long listCount(proto::ProtoContext* ctx, const proto::ProtoObject* l) {
    return static_cast<long long>(asLinkedList(l)->getSize(ctx));
}

const proto::ProtoObject* listNth(proto::ProtoContext* ctx, const proto::ProtoObject* l,
                                  long long index) {
    return orNil(asLinkedList(l)->getAt(ctx, static_cast<proto::proto_ulong>(index)));
}

const proto::ProtoObject* listDrop(proto::ProtoContext* ctx, const proto::ProtoObject* l,
                                   long long count) {
    return asLinkedList(l)->removeFirst(ctx, static_cast<proto::proto_ulong>(count))->asObject(ctx);
}

const proto::ProtoObject* listFromArray(proto::ProtoContext* ctx, std::size_t n,
                                        const proto::ProtoObject* const* items) {
    if (n == 0) return g_seqProtos.empty;
    // protoCore roots the cells while it builds them.
    return withNils(n, items, [&](const proto::ProtoObject* const* values) {
        return ctx->newLinkedListFromArray(n, values)->asObject(ctx);
    });
}

const proto::ProtoObject* listFromProtoList(proto::ProtoContext* ctx, const proto::ProtoList* items) {
    const proto::proto_ulong n = items ? items->getSize(ctx) : 0;
    if (n == 0) return g_seqProtos.empty;
    std::vector<const proto::ProtoObject*> values(n);
    for (proto::proto_ulong i = 0; i < n; ++i) values[i] = orNil(items->getAt(ctx, static_cast<int>(i)));
    return ctx->newLinkedListFromArray(n, values.data())->asObject(ctx);
}

const proto::ProtoList* listToProtoList(proto::ProtoContext* ctx, const proto::ProtoObject* l) {
    if (l == g_seqProtos.empty) return ctx->newList();
    return asLinkedList(l)->asList(ctx);
}

void setSeqToList(SeqToListFn fn) { g_seqToList = fn; }

const proto::ProtoList* seqObjectToList(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
    if (!g_seqToList) throw std::logic_error("seqObjectToList: no runtime hook installed");
    return g_seqToList(ctx, v);
}

} // namespace protoClojure
