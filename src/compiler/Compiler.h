/*
 * Compiler — turns a Clojure form (a runtime value, as the Reader yields it
 * or as a program builds it) into a BytecodeModule.
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
 * The exception is keyword literals, quoted symbols and `:keys` keywords:
 * the compiler interns them (runtime/Named.h) and the module keeps the
 * immortal interned pointer (see the P4 note in BytecodeModule.h).
 *
 * Session 3 scope: integers, strings, symbols (resolved to PUSH_VAR), and
 * lists interpreted as call forms. No special forms yet — that is
 * a later step.
 */
#pragma once
#include "runtime/BytecodeModule.h"
#include "runtime/Named.h"
#include "reader/SourceInfo.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "protoCore.h"  // proto::proto_long / proto_ulong

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

// Attribute keys the Compiler and VM share, and the intern table of keywords
// and symbols. The forms the compiler reads are runtime values (decision M1,
// Reader.h): symbols and keywords are named values, strings are protoCore
// strings, vectors and maps are the runtime's own, so no marker prototype is
// needed to recognise them. A user fn is a small heap object built by the VM
// at MAKE_FN / MAKE_FN_MULTI time; the VM carries the single-arity and
// multi-arity prototypes through ActiveCallContext.
struct CompilerMarkers {
    const proto::ProtoString* bytecodeKey;       // opaque ptr
    const proto::ProtoString* arityKey;          // fn arity
    const proto::ProtoString* capturesKey;       // closure captures list
    const proto::ProtoString* aritiesKey;        // multi-arity dispatch list
    NamedLayout               named;             // keyword and symbol values
};

// True for a qualified symbol `a/b` (a module member): a non-empty prefix
// and member around the first '/'. `/` itself and keywords are not.
bool isQualifiedName(const std::string& name);

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

    // Session 8 — compile a SINGLE arity from a multi-arity fn form. The
    // arity is itself a list whose first element is the params vector
    // (no head symbol). Used by the multi-arity branch of (fn ...) /
    // (defn ...). Same scope-push / capture-cascade semantics as
    // compileFnBody.
    std::unique_ptr<BytecodeModule>
    compileArity(proto::ProtoContext* ctx,
                 const proto::ProtoList* paramsForm,
                 const proto::ProtoList* arityForm,
                 proto::proto_ulong bodyStartIdx,
                 const CompilerMarkers& markers);

    // What the reader recorded about the forms this compiler will compile
    // (SourceInfo.h): the source order of map literals. Optional; without
    // it a map literal is compiled in its walk order. Must outlive the
    // compileForm calls it is set for.
    void setSourceInfo(const SourceInfo* info) { source_ = info; }

private:
    const SourceInfo* source_ = nullptr;

    // The entries of a map form, alternating keys and values, in source
    // order when source_ knows it.
    std::vector<const proto::ProtoObject*>
    mapFormEntries(proto::ProtoContext* ctx, const proto::ProtoObject* m) const;

    // One per active function scope. The outermost compileForm call (top-
    // level) operates with scopes_ empty; `fn` pushes a new scope, the
    // fn-body's locals live there, and the scope is popped when the body
    // finishes. `let` and `loop` extend the current scope's local table.
    struct Scope {
        // name → local slot (params first, then captures introduced on
        // demand, then let/loop bindings — all three live in the same
        // slot space; the BytecodeModule's captureSpecs records which
        // slots are captures, to be populated at CALL time).
        std::unordered_map<std::string, int> nameToSlot;
        int nextSlot = 0;
        int arity    = 0;  // number of params at the start of this scope

        // Session 6 — closure captures recorded for THIS scope. Each
        // entry says: at MAKE_FN time, PUSH_LOCAL parentSlot from the
        // enclosing scope's frame; at CALL time, write the wrapper's
        // captures[i] into localSlot of THIS scope's frame. Copied into
        // the BytecodeModule's captureSpecs_ at compileFnBody exit.
        struct Capture {
            int parentSlot;
            int localSlot;
        };
        std::vector<Capture> captures;

        // recur target — populated by `loop`. The slots vector tells
        // `recur` which locals to rebind, in left-to-right order.
        struct RecurTarget {
            std::size_t  bodyStart;     // instruction position to JUMP_BACK to
            std::vector<int> slots;     // local slot indices for the bindings
            int          tryDepth = 0;  // Scope::tryDepth where the target is
        };
        std::vector<RecurTarget> recurStack;

        // How many `try` forms of this scope enclose the form being compiled.
        // A `recur` whose target sits at a smaller depth would jump out of a
        // try, which Clojure forbids ("Cannot recur across try"): the handler
        // it would leave behind is frame state the jump does not undo.
        int tryDepth = 0;
    };

    // Compiles `(try body* (catch Class name body*)* (finally body*)?)`.
    void compileTry(proto::ProtoContext* ctx, const proto::ProtoList* lst,
                    BytecodeModule& out, const CompilerMarkers& markers);

    // Pushes the type a catch clause names when it is no built-in class,
    // resolved at run time (docs/DESIGN.md §4.1).
    void compileCatchType(proto::ProtoContext* ctx, const std::string& name,
                          BytecodeModule& out);

    // Pushes the receiver `a` of a qualified name `a/b` (a local or a global).
    void compileQualifiedPrefix(proto::ProtoContext* ctx, const std::string& name,
                                BytecodeModule& out);

    // Compiles `(with-open [name init ...] body*)` by rewriting it into
    // `let` + `try` / `finally` forms.
    void compileWithOpen(proto::ProtoContext* ctx, const proto::ProtoList* lst,
                         BytecodeModule& out, const CompilerMarkers& markers);

    std::vector<Scope> scopes_;

    // Resolve `name` against the scope chain. If found in the innermost
    // scope, returns its slot directly. If found in some outer scope,
    // creates a capture chain through every intermediate scope that does
    // not already host `name` (each intermediate scope captures from its
    // own enclosing scope's slot), and returns the slot in the innermost
    // scope. Returns -1 if `name` is not bound in any active scope.
    int resolveLocal(const std::string& name);
};

} // namespace protoClojure
