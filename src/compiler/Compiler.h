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

// The elements of a list form, or of a vector form's items, read once into
// a C++ array so the compiler can index them. Nothing is allocated: the
// elements stay the form's (a list's cells, SeqTypes.h, or a vector's
// ProtoList), and the caller keeps the form rooted while the view is used,
// as it did when it read the form itself. Reads like the ProtoList it
// replaced in the compiler: getAt answers PROTO_NONE past the end.
class FormList {
public:
    FormList() = default;
    // The elements of the Clojure list `form` (SeqTypes.h).
    FormList(proto::ProtoContext* ctx, const proto::ProtoObject* form);
    // The elements of `items` (a vector's ProtoList, or one the compiler
    // built); `form` is what asObject answers.
    FormList(proto::ProtoContext* ctx, const proto::ProtoList* items,
             const proto::ProtoObject* form = nullptr);
    // The given elements, which the caller keeps reachable.
    explicit FormList(std::vector<const proto::ProtoObject*> items)
        : items_(std::move(items)) {}

    proto::proto_ulong getSize(proto::ProtoContext*) const { return items_.size(); }
    const proto::ProtoObject* getAt(proto::ProtoContext*, int i) const {
        if (i < 0) i += static_cast<int>(items_.size());
        return (i >= 0 && static_cast<std::size_t>(i) < items_.size()) ? items_[i] : PROTO_NONE;
    }
    // The form the elements were read from.
    const proto::ProtoObject* asObject(proto::ProtoContext*) const { return form_; }

private:
    const proto::ProtoObject* form_ = nullptr;
    std::vector<const proto::ProtoObject*> items_;
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
                  const FormList* fnForm,
                  const CompilerMarkers& markers);

    // Session 8 — compile a SINGLE arity from a multi-arity fn form. The
    // arity is itself a list whose first element is the params vector
    // (no head symbol). Used by the multi-arity branch of (fn ...) /
    // (defn ...). Same scope-push / capture-cascade semantics as
    // compileFnBody.
    std::unique_ptr<BytecodeModule>
    compileArity(proto::ProtoContext* ctx,
                 const FormList* paramsForm,
                 const FormList* arityForm,
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

    // ---- Debug information (docs/debugging.md) -------------------------
    // Compiled only when the SourceInfo asks for positions (the debugger):
    // a line mark (Op::LINE) before every list form the reader saw, locals'
    // names and scopes, and function names, in the modules that have a
    // DebugInfo. Never inside a defmacro (its body runs at compile time).
    int noDebug_ = 0;
    bool debugOn() const { return source_ && source_->recordPositions && noDebug_ == 0; }
    bool marking(const BytecodeModule& out) const { return debugOn() && out.debugInfo(); }
    // Library code compiled for the debugger keeps locals clearing.
    bool keepsClearing() const { return source_ && source_->library; }
    // The line the reader recorded for the list form `form`, or 0.
    int formLineOf(const proto::ProtoObject* form) const;
    // What the next compileArity is: the fn's name and line, set by the
    // fn / defn / def forms and compileAsThunk, consumed by compileArity.
    std::string pendingFnName_;
    int pendingFnLine_ = 0;
    bool pendingThunk_ = false;
    const proto::ProtoObject* pendingDefExpr_ = nullptr;  // (def name <this>)
    std::string pendingDefName_;
    int lastMarkLine_ = 0;  // the line of the last list form seen

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
    void compileTry(proto::ProtoContext* ctx, const FormList* lst,
                    BytecodeModule& out, const CompilerMarkers& markers);

    // Pushes the type a catch clause names when it is no built-in class,
    // resolved at run time (docs/DESIGN.md §4.1).
    void compileCatchType(proto::ProtoContext* ctx, const std::string& name,
                          BytecodeModule& out);

    // Pushes the receiver `a` of a qualified name `a/b` (a local or a global).
    void compileQualifiedPrefix(proto::ProtoContext* ctx, const std::string& name,
                                BytecodeModule& out);

    std::vector<Scope> scopes_;

    // Debugging helpers (see debugOn above). Out of line, so the frames of
    // compileForm and compileArity -- which nest once per level of source
    // nesting -- do not grow for code compiled without debug information.
    // A local's scope opens at bindLocal and closes at closeLocals.
    std::size_t localsMark(BytecodeModule& out) const;
    void bindLocal(BytecodeModule& out, const std::string& name, int slot,
                   std::size_t startPc) const;
    void closeLocals(BytecodeModule& out, std::size_t from) const;
    // The line mark of the list form `form` whose head is `headName`.
    void markForm(const proto::ProtoObject* form, const std::string& headName,
                  BytecodeModule& out);
    // The name of the fn form `lst` (`nameForm`: defn's name, or nullptr).
    void setPendingFn(proto::ProtoContext* ctx, const FormList* lst,
                      const proto::ProtoObject* nameForm, const proto::ProtoObject* form,
                      const CompilerMarkers& markers, int line);
    // A fn body's debug information and entry mark; its parameters and
    // captures as locals.
    void beginDebugBody(BytecodeModule& body);
    void bindParams(BytecodeModule& body) const;
    void endDebugBody(BytecodeModule& body) const;

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
    void compileDefmacro(proto::ProtoContext* ctx, const FormList* lst,
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
                                                  const FormList* bindings) const;

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
