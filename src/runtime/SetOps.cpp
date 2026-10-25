#include "CallerSlots.h"
#include "SetOps.h"

#include "MapOps.h"

#include "protoCore.h"

#include <vector>

namespace protoClojure {

// Set once per ProtoSpace by initSetOps, before any thread other than the
// initialising one runs Clojure code; rooted by the caller (the globals).
const proto::ProtoObject* g_setPrototype = nullptr;

namespace {

const proto::ProtoString* elementsKey(proto::ProtoContext* ctx) {
    return proto::ProtoString::createSymbol(ctx, "__set__");
}

constexpr proto::proto_ulong kSaltSet = PROTO_UL(0x7C9F2D6A1B3E5F81);

// A new set holding `elements` (a ProtoMap, never null).
const proto::ProtoObject* wrap(proto::ProtoContext* ctx, const proto::ProtoObject* elements) {
    CallerSlots scope(ctx, 1);
    scope.set(0, elements);
    return g_setPrototype->newChild(ctx, /*isMutable=*/false)
        ->setAttribute(ctx, elementsKey(ctx), scope.get(0));
}

} // namespace

const proto::ProtoObject* initSetOps(proto::ProtoContext* ctx) {
    g_setPrototype = ctx->space->objectPrototype->newChild(ctx, /*isMutable=*/false);
    return g_setPrototype;
}


const proto::ProtoObject* setElements(proto::ProtoContext* ctx, const proto::ProtoObject* s) {
    if (!s || s == PROTO_NONE) return nullptr;
    const proto::ProtoObject* m = s->getOwnAttributeDirect(ctx, elementsKey(ctx));
    return (m && m != PROTO_NONE) ? m : nullptr;
}

const proto::ProtoObject* setConj(proto::ProtoContext* ctx, const proto::ProtoObject* base,
                                  const proto::ProtoObject* const* items, proto::proto_ulong n) {
    const proto::ProtoObject* elements = base ? setElements(ctx, base) : nullptr;
    // An element already present keeps its stored object: only absent ones
    // are associated (mapAssocPairs would keep the key but replace the value).
    std::vector<const proto::ProtoObject*> kv;
    kv.reserve(2 * n);
    for (proto::proto_ulong i = 0; i < n; ++i) {
        const proto::ProtoObject* e = items[i] ? items[i] : PROTO_NONE;
        bool found = false;
        if (elements) mapGet(ctx, elements, e, &found);
        if (found) continue;
        kv.push_back(e);
        kv.push_back(e);
    }
    if (base && kv.empty()) return base;
    CallerSlots scope(ctx, 1);
    scope.set(0, mapAssocPairs(ctx, elements, kv.data(), kv.size()));
    return wrap(ctx, scope.get(0));
}

const proto::ProtoObject* setDisj(proto::ProtoContext* ctx, const proto::ProtoObject* s,
                                  const proto::ProtoObject* item) {
    const proto::ProtoObject* elements = setElements(ctx, s);
    if (!elements) return s;
    const proto::ProtoObject* after = mapDissoc(ctx, elements, item ? item : PROTO_NONE);
    if (after == elements) return s;
    CallerSlots scope(ctx, 1);
    scope.set(0, after);
    return wrap(ctx, scope.get(0));
}

const proto::ProtoObject* setGet(proto::ProtoContext* ctx, const proto::ProtoObject* s,
                                 const proto::ProtoObject* item, bool* found) {
    *found = false;
    const proto::ProtoObject* elements = setElements(ctx, s);
    if (!elements) return PROTO_NONE;
    return mapGet(ctx, elements, item ? item : PROTO_NONE, found);
}

proto::proto_ulong setCount(proto::ProtoContext* ctx, const proto::ProtoObject* s) {
    return mapCount(ctx, setElements(ctx, s));
}

bool setIsEmpty(proto::ProtoContext* ctx, const proto::ProtoObject* s) {
    return mapIsEmpty(ctx, setElements(ctx, s));
}

void setForEach(proto::ProtoContext* ctx, const proto::ProtoObject* s, void* self,
                SetElementFn fn) {
    const proto::ProtoObject* elements = setElements(ctx, s);
    if (!elements) return;
    struct Adapter {
        void*        self;
        SetElementFn fn;
    } adapter{self, fn};
    mapForEach(ctx, elements, &adapter,
        [](proto::ProtoContext* c, void* a, const proto::ProtoObject* key,
           const proto::ProtoObject*) {
            const auto* ad = static_cast<Adapter*>(a);
            ad->fn(c, ad->self, key);
        });
}

bool setEquals(proto::ProtoContext* ctx, const proto::ProtoObject* a,
               const proto::ProtoObject* b) {
    if (a == b) return true;
    return mapEquals(ctx, setElements(ctx, a), setElements(ctx, b), nullptr,
        [](proto::ProtoContext*, void*, const proto::ProtoObject*, const proto::ProtoObject*) {
            return true;  // the values are the keys themselves
        });
}

proto::proto_ulong setHash(proto::ProtoContext* ctx, const proto::ProtoObject* s) {
    struct Acc {
        proto::proto_ulong sum = 0;
        proto::proto_ulong n = 0;
    } acc;
    setForEach(ctx, s, &acc, [](proto::ProtoContext* c, void* self, const proto::ProtoObject* e) {
        auto* a = static_cast<Acc*>(self);
        a->sum += keyHash(c, e);  // added: independent of the walk order
        ++a->n;
    });
    return (acc.sum ^ kSaltSet) * PROTO_UL(0x9E3779B97F4A7C15) + acc.n;
}

} // namespace protoClojure
