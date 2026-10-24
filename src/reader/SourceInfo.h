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
 * Debugging (docs/debugging.md). When `recordPositions` is set, the reader
 * also records where every non-empty list form it yields starts -- the
 * forms a program calls, `(f x)`, `(let ...)`, `#(...)` -- and the compiler
 * then compiles debug information into the modules: a line mark (Op::LINE)
 * before each such form, the names of the locals, the function names, and
 * `sourceName` as the modules' source. Only the drivers that run under the
 * debugger (`protoclj --dap`) set it; without it the code is exactly the
 * code compiled before debugging existed. Forms read inside a syntax-quote
 * or a `#_` are not recorded: they do not survive the read, so the cell of
 * one could be reused by a later form.
 *
 * Not thread-safe; one reader and one compiler use it on one thread.
 */
#pragma once

#include <string>
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

    // Debugging only (see above): list form -> its 1-based line and column.
    bool recordPositions = false;
    std::string sourceName;
    struct Position { int line; int column; };
    std::unordered_map<const proto::ProtoObject*, Position> formPos;

    void clear() { mapKeyOrder.clear(); formPos.clear(); }
};

} // namespace protoClojure
