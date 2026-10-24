#include "SeqTypes.h"

#include "protoCore.h"

#include <stdexcept>

namespace protoClojure {

SeqPrototypes g_seqProtos;

namespace {
SeqToListFn g_seqToList = nullptr;
}

const proto::ProtoObject* initSeqTypes(proto::ProtoContext* ctx) {
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(1);
    const proto::ProtoObject* root = ctx->space->objectPrototype;
    const proto::ProtoList* holder = scope.newList();
    for (const proto::ProtoObject** p : {&g_seqProtos.lazy, &g_seqProtos.cons, &g_seqProtos.chunk}) {
        *p = root->newChild(&scope, /*isMutable=*/false);
        holder = holder->appendLast(&scope, *p);
        scope.setAutomaticLocal(0, holder->asObject(&scope));
    }
    scope.returnValue = scope.getAutomaticLocal(0);
    return scope.getAutomaticLocal(0);
}

SeqObject seqObjectKind(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
    if (!v || (reinterpret_cast<proto::proto_ulong>(v) & 0x3F) != 0 || !g_seqProtos.lazy)
        return SeqObject::None;
    const proto::ProtoObject* p = v->getPrototype(ctx);
    if (p == g_seqProtos.lazy)  return SeqObject::Lazy;
    if (p == g_seqProtos.cons)  return SeqObject::Cons;
    if (p == g_seqProtos.chunk) return SeqObject::Chunk;
    return SeqObject::None;
}

void setSeqToList(SeqToListFn fn) { g_seqToList = fn; }

const proto::ProtoList* seqObjectToList(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
    if (!g_seqToList) throw std::logic_error("seqObjectToList: no runtime hook installed");
    return g_seqToList(ctx, v);
}

} // namespace protoClojure
