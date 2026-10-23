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
 * values are equal exactly when they are the same pointer: valuesEqual
 * (Primitives.h) compares them by identity, and a named value is a map key
 * matched by identity (MapOps.h).
 *
 * GC rooting: the runtime roots `marker` and `table` for the lifetime of the
 * ProtoSpace. Every named value is reachable through the table and is never
 * collected, so a named value needs no rooting by its users; spellings are
 * inline strings or strong protoCore symbols, which are never collected
 * either.
 */
#pragma once

#include <atomic>

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

// The fast "is this an interned named value?" test of the map key semantics
// (MapOps.cpp, keyIsIdentity), which sits on every keyword `get` / `assoc`.
// A direct-mapped table of the named values interned so far, indexed by the
// value's cell address. A hit proves `v` is a named value: named values are
// never collected, so a recorded address can never be reused by another
// object. A miss proves nothing (two values may share a slot), and the
// caller falls back to reading the prototype. Lock-free: slots are relaxed
// atomic words, written by internNamed on every call.
namespace namedcache {
constexpr unsigned long kSlots = 1024;
extern std::atomic<const proto::ProtoObject*> slots[kSlots];
inline unsigned long slotOf(const proto::ProtoObject* v) {
    return (reinterpret_cast<unsigned long>(v) >> 6) & (kSlots - 1);
}
inline void record(const proto::ProtoObject* v) {
    std::atomic<const proto::ProtoObject*>& s = slots[slotOf(v)];
    if (s.load(std::memory_order_relaxed) != v) s.store(v, std::memory_order_relaxed);
}
} // namespace namedcache

inline bool isKnownNamed(const proto::ProtoObject* v) {
    return namedcache::slots[namedcache::slotOf(v)].load(std::memory_order_relaxed) == v;
}

// The next number of the process-wide sequence that `gensym` and
// syntax-quote's auto-gensyms (`foo#`) draw from, as Clojure's RT.nextID:
// 1, 2, 3, ... across every thread, so no two generated symbols collide.
// A generated symbol is interned like every symbol and is never collected.
long long nextGensymId();

} // namespace protoClojure
