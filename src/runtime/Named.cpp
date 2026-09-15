#include "Named.h"

#include "protoCore.h"

namespace protoClojure {

const proto::ProtoObject* internNamed(proto::ProtoContext* ctx,
                                      const NamedLayout& layout,
                                      const char* spelling) {
    // createSymbol yields one canonical pointer per spelling (an inline
    // string or a strong symbol). protoCore keys attributes by that pointer
    // in both the read and the compare-and-set below.
    const proto::ProtoString* key = proto::ProtoString::createSymbol(ctx, spelling);
    const proto::ProtoObject* found = layout.table->getOwnAttributeDirect(ctx, key);
    if (found) return found;

    // First use of this spelling: build the value, rooted in a child
    // context until the table holds it.
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(1);
    scope.setAutomaticLocal(0, layout.marker->newChild(&scope, /*isMutable=*/false));
    scope.setAutomaticLocal(0, scope.getAutomaticLocal(0)->setAttribute(
        &scope, layout.spellingKey,
        reinterpret_cast<const proto::ProtoObject*>(key)));
    const proto::ProtoObject* fresh = scope.getAutomaticLocal(0);

    // Publish it, or adopt the value another thread published first.
    for (;;) {
        if (layout.table->setAttributeIfEqual(&scope, key, nullptr, fresh))
            return fresh;
        found = layout.table->getOwnAttributeDirect(&scope, key);
        if (found) return found;
    }
}

bool isNamed(proto::ProtoContext* ctx, const NamedLayout& layout,
             const proto::ProtoObject* v) {
    return v && v != PROTO_NONE && v->getPrototype(ctx) == layout.marker;
}

const proto::ProtoString* namedSpelling(proto::ProtoContext* ctx,
                                        const NamedLayout& layout,
                                        const proto::ProtoObject* v) {
    return reinterpret_cast<const proto::ProtoString*>(
        v->getOwnAttributeDirect(ctx, layout.spellingKey));
}

} // namespace protoClojure
