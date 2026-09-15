#include "MapOps.h"
#include "StackGuard.h"

#include "protoCore.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

namespace protoClojure {

namespace {

// Pointer tags of protoCore's headers/proto_internal.h.
constexpr unsigned long kTagMask         = 0x3F;
constexpr unsigned long kTagObject       = 0;
constexpr unsigned long kTagEmbedded     = 1;
constexpr unsigned long kTagList         = 2;
constexpr unsigned long kTagTuple        = 4;
constexpr unsigned long kTagString       = 6;
constexpr unsigned long kTagLargeInteger = 14;
constexpr unsigned long kTagDouble       = 15;
constexpr unsigned long kTagSymbol       = 22;
constexpr unsigned long kTagListSmall    = 25;

// Hexadecimal digits per big-integer limb: 52 bits, inside the SmallInteger
// range.
constexpr std::size_t kLimbHexDigits = 13;

// The (original-key value) pair stored under each index.
constexpr int kPairKey   = 0;
constexpr int kPairValue = 1;

// Automatic-local slots of a MapBuilder scope.
constexpr unsigned int kSlotMap   = 0;
constexpr unsigned int kSlotPair  = 1;
constexpr unsigned int kSlotCount = 2;

inline unsigned long tagOf(const proto::ProtoObject* v) {
    return reinterpret_cast<unsigned long>(v) & kTagMask;
}

inline const proto::ProtoSparseList* asSparse(const proto::ProtoObject* m) {
    return reinterpret_cast<const proto::ProtoSparseList*>(m);
}

inline unsigned long indexOf(const proto::ProtoObject* canonical) {
    return reinterpret_cast<unsigned long>(canonical);
}

// Appends `v` to the list held in slot 0 of `scope`.
void appendToSlot(proto::ProtoContext& scope, const proto::ProtoObject* v) {
    scope.setAutomaticLocal(0,
        scope.getAutomaticLocal(0)->asList(&scope)->appendLast(&scope, v)
            ->asObject(&scope));
}

// The interned tuple of the list held in slot 0 of `scope`. Interned tuples
// are never collected, so the result needs no rooting once `scope` ends.
const proto::ProtoObject* tupleOfSlot(proto::ProtoContext& scope) {
    return scope.newTupleFromList(scope.getAutomaticLocal(0)->asList(&scope))
        ->asObject(&scope);
}

// A list or a vector: the tuple of the canonical keys of its elements, or
// the vector itself when each element already is its canonical key.
const proto::ProtoObject* canonicalSequence(proto::ProtoContext* ctx,
                                            const MapKeyMarkers& markers,
                                            const proto::ProtoObject* seq,
                                            bool isVector) {
    const proto::ProtoTuple* tuple =
        isVector ? reinterpret_cast<const proto::ProtoTuple*>(seq) : nullptr;
    const proto::ProtoList* list = isVector ? nullptr : seq->asList(ctx);
    const unsigned long n = isVector ? tuple->getSize(ctx) : list->getSize(ctx);
    if (n == 0) {
        // The interned empty tuple: protoCore's ProtoContext::newTuple() (used
        // by `vec` of nil) builds an empty tuple that is not the interned one.
        return ctx->newTupleFromList(ctx->newList())->asObject(ctx);
    }
    auto elementAt = [&](proto::ProtoContext* c, unsigned long i) {
        const int at = static_cast<int>(i);
        return isVector ? tuple->getAt(c, at) : list->getAt(c, at);
    };

    // Slot 0 holds the list of canonical elements once one is needed: from
    // the start for a list, from the first non-canonical element for a
    // vector. Elements are reachable from `seq`; canonical keys are interned
    // or are the element itself.
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(1);
    bool building = !isVector;
    if (building) scope.setAutomaticLocal(0, scope.newList()->asObject(&scope));
    for (unsigned long i = 0; i < n; ++i) {
        const proto::ProtoObject* element = elementAt(&scope, i);
        const proto::ProtoObject* canonical = canonicalKey(&scope, markers, element);
        if (!building && canonical != element) {
            building = true;
            scope.setAutomaticLocal(0, scope.newList()->asObject(&scope));
            for (unsigned long j = 0; j < i; ++j) appendToSlot(scope, elementAt(&scope, j));
        }
        if (building) appendToSlot(scope, canonical);
    }
    return building ? tupleOfSlot(scope) : seq;
}

// A double: (double-marker hi lo), its 64-bit pattern in two 32-bit
// SmallIntegers.
const proto::ProtoObject* canonicalDouble(proto::ProtoContext* ctx,
                                          const MapKeyMarkers& markers,
                                          const proto::ProtoObject* key) {
    const double value = key->asDouble(ctx);
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    const proto::ProtoObject* items[3] = {
        markers.doubleKey,
        ctx->fromLong(static_cast<long long>(bits >> 32)),
        ctx->fromLong(static_cast<long long>(bits & 0xFFFFFFFFULL))};
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(1);
    scope.setAutomaticLocal(0, scope.newList(3, items)->asObject(&scope));
    return tupleOfSlot(scope);
}

// A LargeInteger: (bigint-marker sign limb0 limb1 ...), from the exact
// hexadecimal digits protoCore renders, least significant limb first.
const proto::ProtoObject* canonicalBigInteger(proto::ProtoContext* ctx,
                                              const MapKeyMarkers& markers,
                                              const proto::ProtoObject* key) {
    const std::string hex = key->asIntegerString(ctx, 16)->toStdString(ctx);
    const bool negative = !hex.empty() && hex[0] == '-';
    const std::size_t firstDigit = negative ? 1 : 0;
    const proto::ProtoObject* head[2] = {markers.bigIntegerKey,
                                         ctx->fromLong(negative ? 1 : 0)};
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(1);
    scope.setAutomaticLocal(0, scope.newList(2, head)->asObject(&scope));
    for (std::size_t end = hex.size(); end > firstDigit;) {
        const std::size_t start =
            end - firstDigit > kLimbHexDigits ? end - kLimbHexDigits : firstDigit;
        const long long limb =
            std::strtoll(hex.substr(start, end - start).c_str(), nullptr, 16);
        appendToSlot(scope, scope.fromLong(limb));
        end = start;
    }
    return tupleOfSlot(scope);
}

// A map: (map-marker ck1 cv1 ck2 cv2 ...), in ascending index order, which
// depends only on the set of canonical keys.
const proto::ProtoObject* canonicalMap(proto::ProtoContext* ctx,
                                       const MapKeyMarkers& markers,
                                       const proto::ProtoObject* key) {
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(1);
    const proto::ProtoObject* head[1] = {markers.mapKey};
    scope.setAutomaticLocal(0, scope.newList(1, head)->asObject(&scope));
    struct Walk {
        proto::ProtoContext* scope;
        const MapKeyMarkers* markers;
    } walk{&scope, &markers};
    asSparse(key)->processElements(&scope, &walk,
        [](proto::ProtoContext* c, void* self, unsigned long index,
           const proto::ProtoObject* pair) {
            auto* w = static_cast<Walk*>(self);
            appendToSlot(*w->scope, reinterpret_cast<const proto::ProtoObject*>(index));
            appendToSlot(*w->scope,
                canonicalKey(c, *w->markers, pair->asList(c)->getAt(c, kPairValue)));
        });
    return tupleOfSlot(scope);
}

// A working copy of one map, rooted in the slots of a child context.
// Construct it on the stack only (contexts nest LIFO).
class MapBuilder {
public:
    MapBuilder(proto::ProtoContext* parent, const MapKeyMarkers& markers,
               const proto::ProtoObject* base)
        : scope_(parent->space, parent), markers_(markers) {
        scope_.resizeAutomaticLocals(kSlotCount);
        scope_.setAutomaticLocal(kSlotMap,
            (base && base != PROTO_NONE) ? base
                                         : scope_.newSparseList()->asObject(&scope_));
    }

    proto::ProtoContext* ctx() { return &scope_; }

    // A key already present keeps its stored key object and takes the new
    // value; an identical value changes nothing.
    void assoc(const proto::ProtoObject* key, const proto::ProtoObject* value) {
        if (!key) key = PROTO_NONE;
        if (!value) value = PROTO_NONE;
        const unsigned long index = indexOf(canonicalKey(&scope_, markers_, key));
        const proto::ProtoSparseList* map = slotMap();
        const proto::ProtoObject* existing = map->getAt(&scope_, index);
        const proto::ProtoObject* pair[2] = {key, value};
        if (existing != PROTO_NONE) {
            const proto::ProtoList* stored = existing->asList(&scope_);
            if (stored->getAt(&scope_, kPairValue) == value) return;
            pair[kPairKey] = stored->getAt(&scope_, kPairKey);
        }
        scope_.setAutomaticLocal(kSlotPair, scope_.newList(2, pair)->asObject(&scope_));
        scope_.setAutomaticLocal(kSlotMap,
            map->setAt(&scope_, index, scope_.getAutomaticLocal(kSlotPair))
                ->asObject(&scope_));
    }

    // Returns false, changing nothing, when `key` is absent.
    bool dissoc(const proto::ProtoObject* key) {
        const unsigned long index =
            indexOf(canonicalKey(&scope_, markers_, key ? key : PROTO_NONE));
        const proto::ProtoSparseList* map = slotMap();
        if (map->getAt(&scope_, index) == PROTO_NONE) return false;
        scope_.setAutomaticLocal(kSlotMap, map->removeAt(&scope_, index)->asObject(&scope_));
        return true;
    }

    // The map built so far. UNROOTED once this builder goes out of scope.
    const proto::ProtoObject* result() { return scope_.getAutomaticLocal(kSlotMap); }

private:
    const proto::ProtoSparseList* slotMap() {
        return asSparse(scope_.getAutomaticLocal(kSlotMap));
    }

    proto::ProtoContext  scope_;
    const MapKeyMarkers& markers_;
};

void requireEven(unsigned long n) {
    if (n % 2 != 0)
        throw std::runtime_error("map: a key has no value");
}

} // namespace

const proto::ProtoObject* canonicalKey(proto::ProtoContext* ctx,
                                       const MapKeyMarkers& markers,
                                       const proto::ProtoObject* key) {
    if (!key) return PROTO_NONE;
    const unsigned long tag = tagOf(key);
    if (tag == kTagSymbol) return key;
    // Every other string, inline strings included: protoCore stores a short
    // non-ASCII string inline or as a string cell depending on the operation
    // that produced it, while createSymbol returns the inline form only for
    // up to 6 ASCII bytes. createSymbol gives equal contents one pointer and
    // allocates nothing for short ASCII strings.
    if (proto::ProtoObject::isStringTagFast(key)) {
        return reinterpret_cast<const proto::ProtoObject*>(
            proto::ProtoString::createSymbol(
                ctx, reinterpret_cast<const proto::ProtoString*>(key)->toStdString(ctx)));
    }
    switch (tag) {
        case kTagEmbedded:
        case kTagObject:
            return key;
        case kTagDouble:
            return canonicalDouble(ctx, markers, key);
        case kTagLargeInteger:
            return canonicalBigInteger(ctx, markers, key);
        case kTagTuple:
            checkNativeStack();
            return canonicalSequence(ctx, markers, key, /*isVector=*/true);
        case kTagList:
        case kTagListSmall:
            checkNativeStack();
            return canonicalSequence(ctx, markers, key, /*isVector=*/false);
        default:
            if (isMap(key)) {
                checkNativeStack();
                return canonicalMap(ctx, markers, key);
            }
            return key;
    }
}

const proto::ProtoObject* mapAssocPairs(proto::ProtoContext* ctx,
                                        const MapKeyMarkers& markers,
                                        const proto::ProtoObject* base,
                                        const proto::ProtoObject* const* kv,
                                        unsigned long n) {
    requireEven(n);
    MapBuilder b(ctx, markers, base);
    for (unsigned long i = 0; i < n; i += 2) b.assoc(kv[i], kv[i + 1]);
    return b.result();
}

const proto::ProtoObject* mapAssocPairs(proto::ProtoContext* ctx,
                                        const MapKeyMarkers& markers,
                                        const proto::ProtoObject* base,
                                        const proto::ProtoList* items,
                                        unsigned long from,
                                        unsigned long n) {
    requireEven(n);
    MapBuilder b(ctx, markers, base);
    for (unsigned long i = from; i < from + n; i += 2) {
        b.assoc(items->getAt(b.ctx(), static_cast<int>(i)),
                items->getAt(b.ctx(), static_cast<int>(i + 1)));
    }
    return b.result();
}

const proto::ProtoObject* mapDissoc(proto::ProtoContext* ctx,
                                    const MapKeyMarkers& markers,
                                    const proto::ProtoObject* m,
                                    const proto::ProtoObject* key) {
    if (mapCount(ctx, m) == 0) return m;
    MapBuilder b(ctx, markers, m);
    if (!b.dissoc(key)) return m;
    return b.result();
}

const proto::ProtoObject* mapGet(proto::ProtoContext* ctx,
                                 const MapKeyMarkers& markers,
                                 const proto::ProtoObject* m,
                                 const proto::ProtoObject* key,
                                 bool* found) {
    *found = false;
    if (!m || m == PROTO_NONE) return PROTO_NONE;
    const proto::ProtoObject* pair =
        asSparse(m)->getAt(ctx, indexOf(canonicalKey(ctx, markers, key)));
    if (pair == PROTO_NONE) return PROTO_NONE;
    *found = true;
    return pair->asList(ctx)->getAt(ctx, kPairValue);
}

unsigned long mapCount(proto::ProtoContext* ctx, const proto::ProtoObject* m) {
    return (!m || m == PROTO_NONE) ? 0 : asSparse(m)->getSize(ctx);
}

void mapForEach(proto::ProtoContext* ctx, const proto::ProtoObject* m,
                void* self, MapEntryFn fn) {
    if (!m || m == PROTO_NONE) return;
    // Pin the map: a callback may replace the only other reference to it.
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(1);
    scope.setAutomaticLocal(0, m);
    struct Adapter {
        void*      self;
        MapEntryFn fn;
    } adapter{self, fn};
    asSparse(m)->processElements(&scope, &adapter,
        [](proto::ProtoContext* c, void* a, unsigned long /*index*/,
           const proto::ProtoObject* pairObj) {
            const auto* ad = static_cast<Adapter*>(a);
            const proto::ProtoList* pair = pairObj->asList(c);
            ad->fn(c, ad->self, pair->getAt(c, kPairKey), pair->getAt(c, kPairValue));
        });
}

bool mapEquals(proto::ProtoContext* ctx,
               const proto::ProtoObject* a, const proto::ProtoObject* b,
               void* self, MapValueEqFn valueEq) {
    if (a == b) return true;
    const unsigned long n = mapCount(ctx, a);
    if (n != mapCount(ctx, b)) return false;
    if (n == 0) return true;

    // Equal counts plus every canonical key of `a` present in `b` means the
    // key sets are equal.
    struct Walk {
        const proto::ProtoSparseList* other;
        void*                         self;
        MapValueEqFn                  valueEq;
        bool                          equal;
    } walk{asSparse(b), self, valueEq, true};
    asSparse(a)->processElements(ctx, &walk,
        [](proto::ProtoContext* c, void* p, unsigned long index,
           const proto::ProtoObject* pairObj) {
            auto* w = static_cast<Walk*>(p);
            if (!w->equal) return;  // processElements cannot stop early
            const proto::ProtoObject* otherPair = w->other->getAt(c, index);
            if (otherPair == PROTO_NONE) { w->equal = false; return; }
            w->equal = w->valueEq(c, w->self,
                                  pairObj->asList(c)->getAt(c, kPairValue),
                                  otherPair->asList(c)->getAt(c, kPairValue));
        });
    return walk.equal;
}

} // namespace protoClojure
