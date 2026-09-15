/*
 * Named — keywords and symbols as runtime values.
 *
 * A keyword literal (`:a`) and a quoted symbol (`(quote a)`) evaluate to a
 * named value: an immutable child of NamedLayout::marker whose own attribute
 * NamedLayout::spellingKey holds its spelling as a protoCore symbol — the
 * source text, with the leading `:` for a keyword. Keywords and symbols
 * share the representation; the spelling tells them apart, since a symbol
 * never starts with `:`.
 *
 * Why not a bare ProtoString. Strings are protoCore strings, and protoCore
 * stores a short ASCII string (up to 7 bytes) inline in the tagged pointer,
 * so `createSymbol(":a")` and `fromUTF8String(":a")` are the same pointer;
 * longer ones compare equal by content. A keyword represented as a symbol
 * was therefore `=` to the string of its spelling, matched it as a map key
 * and satisfied `string?`. A named value is never a string.
 *
 * Interning. NamedLayout::table, a mutable object, maps each spelling
 * symbol to its named value. internNamed publishes a new value with
 * setAttributeIfEqual (a lock-free compare-and-set), so threads interning
 * the same spelling concurrently all obtain the pointer that won. Two named
 * values are equal exactly when they are the same pointer: valuesEqual and
 * valueHash (Primitives.h) handle them through their identity fallbacks.
 *
 * GC rooting: the runtime roots `marker` and `table` for the lifetime of the
 * ProtoSpace. Every named value is reachable through the table and is never
 * collected, so a named value needs no rooting by its users; spellings are
 * inline strings or strong protoCore symbols, which are never collected
 * either.
 */
#pragma once

namespace proto {
class ProtoContext;
class ProtoObject;
class ProtoString;
}

namespace protoClojure {

struct NamedLayout {
    const proto::ProtoObject* marker = nullptr;       // prototype of every named value
    const proto::ProtoObject* table = nullptr;        // mutable: spelling -> named value
    const proto::ProtoString* spellingKey = nullptr;  // own attribute holding the spelling
};

// The interned named value spelled `spelling` (":a" for the keyword :a, "a"
// for the symbol a). The same spelling returns the same pointer on every
// thread.
//
// Cost: one createSymbol and one own-attribute read of the (mutable) table;
// the first use of a spelling also allocates the value and publishes it.
// Too slow for a per-instruction path: the compiler interns keyword
// literals, quoted symbols and `:keys` keywords once and the bytecode keeps
// the pointer (BytecodeModule.h), so executing them costs a pointer read.
const proto::ProtoObject* internNamed(proto::ProtoContext* ctx,
                                      const NamedLayout& layout,
                                      const char* spelling);

// True when `v` is a named value (a keyword or a symbol).
bool isNamed(proto::ProtoContext* ctx, const NamedLayout& layout,
             const proto::ProtoObject* v);

// The spelling of the named value `v` (`isNamed` must hold).
const proto::ProtoString* namedSpelling(proto::ProtoContext* ctx,
                                        const NamedLayout& layout,
                                        const proto::ProtoObject* v);

} // namespace protoClojure
