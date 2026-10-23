/*
 * SourceInfo — what the reader knows about the source of the forms it
 * yielded and the values themselves cannot carry.
 *
 * The reader yields runtime values (Reader.h), and a runtime map has no
 * order: its walk order is unspecified (runtime/MapOps.h). A map literal's
 * forms are nevertheless evaluated left to right in source order, as in
 * Clojure for every literal up to eight entries (an array map there), so
 * `{:a (f) :b (g)}` calls `f` before `g`. The reader records the keys of
 * every map literal in source order here, keyed by the map it yielded, and
 * the compiler compiles the literal in that order. A map the reader did not
 * yield (one a macro builds, stage 4) is compiled in its walk order, which
 * is what Clojure does for a hash map.
 *
 * Lifetime. The table holds raw pointers to forms, which is safe only while
 * those forms are alive: protoCore does not move objects, but it reuses the
 * cells of dead ones. A driver therefore keeps one SourceInfo per unit it
 * reads and compiles (a top-level form, or a file whose forms stay rooted),
 * and clears it before the forms it describes can be collected.
 *
 * Not thread-safe; one reader and one compiler use it on one thread.
 */
#pragma once

#include <unordered_map>
#include <vector>

namespace proto {
class ProtoObject;
}

namespace protoClojure {

struct SourceInfo {
    // Map literal -> its keys in source order.
    std::unordered_map<const proto::ProtoObject*,
                       std::vector<const proto::ProtoObject*>> mapKeyOrder;

    void clear() { mapKeyOrder.clear(); }
};

} // namespace protoClojure
