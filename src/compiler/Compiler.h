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

    // Expands `form` while it is a macro call, with no locals in scope (a
    // top-level form), and answers the result: `form` itself when its head
    // names no macro. Each expansion is anchored in `ctx` (Macros.h), which
    // the caller keeps alive until it has rooted the result. A macro that
    // raises, or is called with the wrong number of arguments, is a
    // CompileError naming it. The drivers expand a top-level form this way
    // before compiling it so that a `do` the expansion yields is evaluated
    // one form at a time (Eval.h).
    const proto::ProtoObject* macroexpandTopLevel(proto::ProtoContext* ctx,
                                                  const proto::ProtoObject* form,
                                                  const CompilerMarkers& markers);

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

    std::vector<Scope> scopes_;

    // The bindings a `let` or `loop` introduces in the current scope, undone
    // when the form has been compiled: each name gets a fresh slot, and the
    // slot it shadowed (or none) is restored on destruction, so a binding is
    // visible in its own body only. Holds the scope by index: compiling the
    // body may grow scopes_.
    struct BindingRestore {
        std::vector<Scope>& scopes;
        std::size_t idx;
        std::vector<std::pair<std::string, int>> saved;  // name, shadowed slot or -1
        explicit BindingRestore(std::vector<Scope>& s) : scopes(s), idx(s.size() - 1) {}
        int bind(const std::string& name) {
            Scope& scope = scopes[idx];
            auto it = scope.nameToSlot.find(name);
            saved.emplace_back(name, it == scope.nameToSlot.end() ? -1 : it->second);
            const int slot = scope.nextSlot++;
            scope.nameToSlot[name] = slot;
            return slot;
        }
        ~BindingRestore() {
            if (idx >= scopes.size()) return;
            auto& names = scopes[idx].nameToSlot;
            for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
                if (it->second >= 0) names[it->first] = it->second;
                else names.erase(it->first);
            }
        }
    };

    // True when `name` is bound as a local in any active scope. Unlike
    // resolveLocal it creates no capture.
    bool isLocalName(const std::string& name) const;

    // The macro the head of a call names, or nullptr: the head is a symbol
    // that is not a special form and not a local (`user/x` is never a
    // local), and the global it names holds a macro (Macros.h). `name`
    // receives the head's spelling.
    const proto::ProtoObject* macroForHead(proto::ProtoContext* ctx,
                                           const proto::ProtoObject* head,
                                           const CompilerMarkers& markers,
                                           std::string& name) const;

    // `&env` for a macro called here: nil at the top level, otherwise a map
    // from each local's symbol to itself (Clojure maps it to the compiler's
    // LocalBinding object, which protoClojure does not have).
    const proto::ProtoObject* localEnv(proto::ProtoContext* ctx,
                                       const CompilerMarkers& markers) const;

    // Expands the macro call `form` (head naming `macro`) and compiles the
    // expansion in its place, recursively: an expansion that is itself a
    // macro call is expanded again.
    void compileMacroCall(proto::ProtoContext* ctx, const proto::ProtoObject* form,
                          const proto::ProtoObject* macro, const std::string& name,
                          BytecodeModule& out, const CompilerMarkers& markers);

    // Compiles `(defmacro name doc? attr-map? [params] body*)` or its
    // multi-arity form as `(def name (__macro__ (fn [&form &env params] body*)))`.
    void compileDefmacro(proto::ProtoContext* ctx, const proto::ProtoList* lst,
                         BytecodeModule& out, const CompilerMarkers& markers);

    // Destructuring (docs/DESIGN.md §10.1, M23), as clojure.core/destructure
    // does it: appends to `out` the symbol/expression pairs that bind the
    // binding form `pattern` (a symbol, a vector or a map form) to the value
    // of `value`, in order. The expressions it builds call clojure.core/nth,
    // first, next, seq, get and __destructure-map on generated locals
    // (vec__N, seq__N, first__N, map__N). The forms are allocated in `ctx`,
    // which the caller keeps alive (and roots the result in) until they are
    // compiled. An unsupported form is a CompileError with Clojure's message.
    void destructure(proto::ProtoContext* ctx, const CompilerMarkers& markers,
                     const proto::ProtoObject* pattern, const proto::ProtoObject* value,
                     std::vector<const proto::ProtoObject*>& out) const;

    // The binding vector `bindings` (pattern/value pairs) with every pattern
    // destructured into symbols: a vector form of symbol/value pairs, built in
    // `ctx`, or nullptr when every pattern already is a symbol.
    const proto::ProtoObject* destructureBindings(proto::ProtoContext* ctx,
                                                  const CompilerMarkers& markers,
                                                  const proto::ProtoList* bindings) const;

    // Compiles `form` as the body of a zero-argument fn called on the spot,
    // `((fn [] form))`: how a top-level form that needs local slots (`let`,
    // `loop`, `try`) runs, since the top level has none.
    void compileAsThunk(proto::ProtoContext* ctx, const proto::ProtoObject* form,
                        BytecodeModule& out, const CompilerMarkers& markers);

    // Resolve `name` against the scope chain. If found in the innermost
    // scope, returns its slot directly. If found in some outer scope,
    // creates a capture chain through every intermediate scope that does
    // not already host `name` (each intermediate scope captures from its
    // own enclosing scope's slot), and returns the slot in the innermost
    // scope. Returns -1 if `name` is not bound in any active scope.
    int resolveLocal(const std::string& name);
};

} // namespace protoClojure
