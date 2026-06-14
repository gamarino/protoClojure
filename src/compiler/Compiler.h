/*
 * Compiler — turns a Clojure form (a ProtoObject tree from the Reader) into
 * a BytecodeModule.
 *
 * P1/P2: the compiler receives a ProtoContext from its caller and walks
 * the form's children. Every intermediate ProtoObject* it inspects is
 * either a parameter (rooted by the caller's slot, P1) or a result of
 * getAttribute/asList/getAt on an already-rooted parameter (returns from
 * protoCore methods that are immediately used — we do not hold them
 * across allocations). No child contexts pushed in v0.0.x — the compiler
 * is non-recursive in the protoCore-allocation sense; it consumes existing
 * forms and emits bytecode.
 *
 * P3: writes go to a BytecodeModule, which is C++-side. Const-pool values
 * are POD (long long) or std::string; they never enter protoCore until
 * the VM materialises them at PUSH_CONST time, via raw-bytes c_str().
 *
 * Session 3 scope: integers, strings, symbols (resolved to PUSH_VAR), and
 * lists interpreted as call forms. No special forms yet — that is
 * session 4.
 */
#pragma once
#include "runtime/BytecodeModule.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace proto {
class ProtoContext;
class ProtoObject;
class ProtoString;
class ProtoList;
}

namespace protoClojure {

struct CompileError : std::runtime_error {
    CompileError(const std::string& msg) : std::runtime_error(msg) {}
};

// Marker prototypes + attribute keys the Compiler and VM need. The same
// wrapping idea as ReaderMarkers: a user fn is a small heap object whose
// prototype is fnMarkerProto with the bytecode pointer + arity stored as
// attributes. The Compiler emits MAKE_FN; the VM constructs the wrapper.
struct CompilerMarkers {
    const proto::ProtoObject* stringMarkerProto;
    const proto::ProtoObject* fnMarkerProto;     // session 5
    const proto::ProtoString* bytesKey;
    const proto::ProtoString* bytecodeKey;       // session 5 — opaque ptr
    const proto::ProtoString* arityKey;          // session 5
};

class Compiler {
public:
    // Compile a single form, appending instructions to `out`. Does NOT
    // terminate the module — the file runner calls `compileForm` once per
    // top-level form, then appends a final RETURN.
    void compileForm(proto::ProtoContext* ctx,
                     const proto::ProtoObject* form,
                     BytecodeModule& out,
                     const CompilerMarkers& markers);

    // Session 5: compile a Clojure `fn [params] body...` form into its OWN
    // BytecodeModule (returned). The caller — typically the def/fn special
    // forms inside compileForm — receives ownership and either embeds it as
    // a sub-block of an outer module via addBlock, or stores it elsewhere.
    std::unique_ptr<BytecodeModule>
    compileFnBody(proto::ProtoContext* ctx,
                  const proto::ProtoList* fnForm,
                  const CompilerMarkers& markers);

private:
    // One per active function scope. The outermost compileForm call (top-
    // level) operates with scopes_ empty; `fn` pushes a new scope, the
    // fn-body's locals live there, and the scope is popped when the body
    // finishes. `let` and `loop` extend the current scope's local table.
    struct Scope {
        // name → local slot (params first, then let/loop bindings).
        std::unordered_map<std::string, int> nameToSlot;
        int nextSlot = 0;
        int arity    = 0;  // number of params at the start of this scope

        // recur target — populated by `loop` (or by `fn` once we add
        // implicit-loop-around-fn semantics in session 5+). The slots
        // vector tells `recur` which locals to rebind, in left-to-right
        // order. Empty stack = recur is illegal at the current position.
        struct RecurTarget {
            std::size_t  bodyStart;     // byte offset (PC) to JUMP_BACK to
            std::vector<int> slots;     // local slot indices for the bindings
        };
        std::vector<RecurTarget> recurStack;
    };

    std::vector<Scope> scopes_;

    // Look up `name` in the innermost scope (the top of scopes_). Returns
    // -1 if not found. For session 5 we do NOT walk up the scope chain —
    // closures land in a later session, and nested fns capture nothing yet.
    int lookupLocal(const std::string& name) const;
};

} // namespace protoClojure
