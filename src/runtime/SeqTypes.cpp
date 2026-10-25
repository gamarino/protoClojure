#include "CallerSlots.h"
#include "SeqTypes.h"

#include "ListBuilder.h"

#include "protoCore.h"

#include <stdexcept>

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
    for (const proto::ProtoObject** p : {&g_seqProtos.lazy, &g_seqProtos.cons, &g_seqProtos.chunk,
                                         &g_seqProtos.list, &g_seqProtos.empty}) {
        *p = root->newChild(ctx, /*isMutable=*/false);
        holder = holder->appendLast(ctx, *p);
        scope.set(0, holder->asObject(ctx));
    }
    return scope.get(0);
}

SeqObject seqObjectKind(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
    if (!v || (reinterpret_cast<proto::proto_ulong>(v) & 0x3F) != 0 || !g_seqProtos.lazy)
        return SeqObject::None;
    if (v == g_seqProtos.empty) return SeqObject::Empty;
    const proto::ProtoObject* p = v->getPrototype(ctx);
    if (p == g_seqProtos.list)  return SeqObject::List;
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

bool isListValue(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
    if (!v || (reinterpret_cast<proto::proto_ulong>(v) & 0x3F) != 0 || !g_seqProtos.empty)
        return false;
    return v == g_seqProtos.empty || v->getPrototype(ctx) == g_seqProtos.list;
}

const proto::ProtoObject* listCons(proto::ProtoContext* ctx, const proto::ProtoObject* x,
                                   const proto::ProtoObject* rest) {
    const proto::ProtoObject* fields[3] = {orNil(x), rest,
                                           proto::makeSmallInt(listCount(ctx, rest) + 1)};
    return newSeqNode(ctx, g_seqProtos.list, 3, fields);
}

const proto::ProtoObject* listFirst(proto::ProtoContext* ctx, const proto::ProtoObject* cell) {
    return orNil(seqNodeFields(ctx, cell)->getAt(ctx, 0));
}

const proto::ProtoObject* listRest(proto::ProtoContext* ctx, const proto::ProtoObject* cell) {
    return seqNodeFields(ctx, cell)->getAt(ctx, 1);
}

void listSplit(proto::ProtoContext* ctx, const proto::ProtoObject* cell,
               const proto::ProtoObject** first, const proto::ProtoObject** rest) {
    const proto::ProtoList* f = seqNodeFields(ctx, cell);
    *first = orNil(f->getAt(ctx, 0));
    *rest = f->getAt(ctx, 1);
}

long long listCount(proto::ProtoContext* ctx, const proto::ProtoObject* l) {
    if (l == g_seqProtos.empty) return 0;
    return proto::asSmallInt(seqNodeFields(ctx, l)->getAt(ctx, 2));
}

const proto::ProtoObject* listFromArray(proto::ProtoContext* ctx, std::size_t n,
                                        const proto::ProtoObject* const* items) {
    if (n == 0) return g_seqProtos.empty;
    // Built from the end; each new cell is rooted in the slot, and holds the
    // ones built before it. Nothing built here becomes garbage.
    CallerSlots scope(ctx, 1);
    const proto::ProtoObject* acc = g_seqProtos.empty;
    for (std::size_t i = n; i-- > 0;) {
        acc = listCons(ctx, items[i], acc);
        scope.set(0, acc);
    }
    return acc;
}

const proto::ProtoObject* listFromProtoList(proto::ProtoContext* ctx, const proto::ProtoList* items) {
    const proto::proto_ulong n = items ? items->getSize(ctx) : 0;
    if (n == 0) return g_seqProtos.empty;
    CallerSlots scope(ctx, 1);
    const proto::ProtoObject* acc = g_seqProtos.empty;
    for (proto::proto_ulong i = n; i-- > 0;) {
        acc = listCons(ctx, items->getAt(ctx, static_cast<int>(i)), acc);
        scope.set(0, acc);
    }
    return acc;
}

const proto::ProtoList* listToProtoList(proto::ProtoContext* ctx, const proto::ProtoObject* l) {
    if (l == g_seqProtos.empty) return ctx->newList();
    // The list is rooted by the caller, and so is every cell of it the walk
    // reads; the builder roots what it has built.
    const proto::ProtoObject* out;
    {
        ListBuilder b(ctx);
        for (const proto::ProtoObject* p = l; p != g_seqProtos.empty; p = listRest(b.context(), p))
            b.push(listFirst(b.context(), p));
        out = b.finish();
    }
    return out->asList(ctx);
}

void setSeqToList(SeqToListFn fn) { g_seqToList = fn; }

const proto::ProtoList* seqObjectToList(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
    if (!g_seqToList) throw std::logic_error("seqObjectToList: no runtime hook installed");
    return g_seqToList(ctx, v);
}

} // namespace protoClojure
