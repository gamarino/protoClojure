#include "CallerSlots.h"
#include "Named.h"

#include "protoCore.h"

#include <atomic>
#include <unordered_map>

namespace protoClojure {

namespace namedcache {
std::atomic<const proto::ProtoObject*> slots[kSlots];
}

std::uint64_t newNamedEpoch() {
    static std::atomic<std::uint64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}

namespace {

// The per-thread caches of internSymbol, internNamed and namedSpellingText.
// Plain C++ data on each thread: the values they hold (protoCore symbols and
// named values) are never collected during a run, and the epoch check
// empties them when a thread starts serving another run. Each map is
// bounded: a program that interns an unbounded number of spellings (a
// keyword per input line, say) empties it when it grows past kMaxEntries,
// and the next lookups fill it again.
struct NamedCaches {
    static constexpr std::size_t kMaxEntries = 1u << 16;
    std::uint64_t epoch = 0;
    const proto::ProtoObject* tmpl = nullptr;
    std::unordered_map<std::string, const proto::ProtoString*> symbols;
    std::unordered_map<std::string, const proto::ProtoObject*> named;
    std::unordered_map<const proto::ProtoObject*, std::string> spellings;
};

NamedCaches& cachesFor(const NamedLayout& layout) {
    thread_local NamedCaches caches;
    if (caches.epoch != layout.epoch) {
        caches.tmpl = nullptr;
        caches.symbols.clear();
        caches.named.clear();
        caches.spellings.clear();
        caches.epoch = layout.epoch;
    }
    return caches;
}

template <typename Map>
void bound(Map& m) {
    if (m.size() >= NamedCaches::kMaxEntries) m.clear();
}

} // namespace

const proto::ProtoString* internSymbol(proto::ProtoContext* ctx,
                                       const NamedLayout& layout,
                                       const std::string& text) {
    NamedCaches& c = cachesFor(layout);
    auto it = c.symbols.find(text);
    if (it != c.symbols.end()) return it->second;
    const proto::ProtoString* sym = proto::ProtoString::createSymbol(ctx, text.c_str());
    bound(c.symbols);
    c.symbols.emplace(text, sym);
    return sym;
}

const std::string& namedSpellingText(proto::ProtoContext* ctx,
                                     const NamedLayout& layout,
                                     const proto::ProtoObject* v) {
    NamedCaches& c = cachesFor(layout);
    auto it = c.spellings.find(v);
    if (it != c.spellings.end()) return it->second;
    bound(c.spellings);
    return c.spellings.emplace(v, namedSpelling(ctx, layout, v)->toStdString(ctx)).first->second;
}

static const proto::ProtoObject* internNamedUncached(proto::ProtoContext* ctx,
                                                     const NamedLayout& layout,
                                                     const char* spelling);

const proto::ProtoObject* internNamed(proto::ProtoContext* ctx,
                                      const NamedLayout& layout,
                                      const char* spelling) {
    NamedCaches& c = cachesFor(layout);
    std::string text(spelling);
    auto it = c.named.find(text);
    if (it != c.named.end()) { namedcache::record(it->second); return it->second; }
    const proto::ProtoObject* v = internNamedUncached(ctx, layout, spelling);
    bound(c.named);
    c.named.emplace(std::move(text), v);
    return v;
}

static const proto::ProtoObject* internNamedUncached(proto::ProtoContext* ctx,
                                                     const NamedLayout& layout,
                                                     const char* spelling) {
    // createSymbol yields one canonical pointer per spelling (an inline
    // string or a strong symbol). protoCore keys attributes by that pointer
    // in both the read and the compare-and-set below.
    const proto::ProtoString* key = proto::ProtoString::createSymbol(ctx, spelling);
    const proto::ProtoObject* found = layout.table->getOwnAttributeDirect(ctx, key);
    if (found) { namedcache::record(found); return found; }

    // First use of this spelling: derive from the space's pre-shaped named
    // template (rooted in layout.table under "\x01tmpl") so all keywords and
    // symbols share one ProtoShape* and fit in a single 64-byte cell.
    NamedCaches& c = cachesFor(layout);
    if (!c.tmpl) {
        const proto::ProtoString* tmplKey = proto::ProtoString::createSymbol(ctx, "\x01tmpl");
        const proto::ProtoObject* t = layout.table->getOwnAttributeDirect(ctx, tmplKey);
        if (!t) {
            CallerSlots tscope(ctx, 1);
            tscope.set(0, layout.marker->newChild(ctx, /*isMutable=*/false)
                ->setAttribute(ctx, layout.spellingKey, PROTO_NONE)
                ->removeAttribute(ctx, layout.spellingKey));
            layout.table->setAttributeIfEqual(ctx, tmplKey, nullptr, tscope.get(0));
            t = layout.table->getOwnAttributeDirect(ctx, tmplKey);
        }
        c.tmpl = t;
    }
    CallerSlots scope(ctx, 1);
    scope.set(0, c.tmpl->setAttribute(
        ctx, layout.spellingKey,
        reinterpret_cast<const proto::ProtoObject*>(key)));
    const proto::ProtoObject* fresh = scope.get(0);

    // Publish it, or adopt the value another thread published first.
    for (;;) {
        if (layout.table->setAttributeIfEqual(ctx, key, nullptr, fresh)) {
            namedcache::record(fresh);
            return fresh;
        }
        found = layout.table->getOwnAttributeDirect(ctx, key);
        if (found) { namedcache::record(found); return found; }
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

long long nextGensymId() {
    static std::atomic<long long> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}

} // namespace protoClojure
