/*
 * Macros — the run-time side of `defmacro` (docs/DESIGN.md §10).
 *
 * Representation. A macro is an ordinary user fn whose wrapper carries the
 * own attribute `__macro__` (true), stored in the globals under its name like
 * any `def`. Its first two parameters are Clojure's hidden `&form` (the whole
 * macro call, unevaluated) and `&env` (the locals in scope at the call), so a
 * macro is called as `(m form env arg...)`. `defmacro` compiles to
 * `(def name (__macro__ (fn [&form &env params...] body...)))`.
 *
 * Because the flag lives on the value, the macro table is the globals table:
 * a `def` of the same name replaces the macro with a plain value, a second
 * `defmacro` replaces it with the new macro, and the compiler, which looks
 * the name up when it compiles each call, sees whichever definition last
 * ran. Forms compiled before a redefinition keep the expansion they got.
 *
 * Expansion happens at compile time (Compiler.cpp) and on demand through
 * `macroexpand-1` / `macroexpand`; both go through expandMacroCall below.
 * The macro runs on the evaluator thread's installed call context
 * (ActiveCallContext), so a driver must keep one installed while it
 * compiles.
 */
#pragma once

#include <string>

namespace proto {
class ProtoContext;
class ProtoObject;
class ProtoString;
}

namespace protoClojure {

struct ActiveCallContext;

// The own attribute that marks a fn wrapper as a macro.
const proto::ProtoString* macroFlagKey(proto::ProtoContext* ctx);

// True when `v` is a macro: a user fn wrapper carrying the macro flag.
bool isMacroValue(proto::ProtoContext* ctx, const ActiveCallContext& cc,
                  const proto::ProtoObject* v);

// The names Clojure treats as special forms, which are never macro calls
// and which syntax-quote leaves unqualified: def if do let* loop* fn* quote
// var recur throw try catch finally new set! letfn* case* . & , plus `let`,
// `loop` and `fn`, which protoClojure's compiler implements directly as
// let* / loop* / fn*.
bool isSpecialFormName(const std::string& name);

// The globals key a symbol names when it denotes a global (D28: there are
// no namespaces, only the globals): `user/x` names the global `x` (the
// program's own), `clojure.core/x` the key `clojure.core/x`, where every
// built-in is also installed (defineBuiltin); any other spelling is its own
// key. Returns false (leaving `key` unspecified) for a keyword.
bool globalKeyForSymbol(const std::string& spelling, std::string& key);

// Installs `value` as the built-in `name`: under `name`, which the program
// may redefine, and under `clojure.core/name`, which a `def` of `name` does
// not touch, so what syntax-quote expansions call stays the built-in.
void defineBuiltin(proto::ProtoContext* ctx, proto::ProtoObject* globals,
                   const char* name, const proto::ProtoObject* value);

// Namespace aliases (docs/DESIGN.md §10.2, M33): `(require '[clojure.string
// :as str])`, and `ns` with `(:require ...)`, record that `str` stands for
// `clojure.string`, so the compiler reads `str/join` as `clojure.string/join`
// in the forms compiled afterwards. There are no namespaces (D28): the table
// is one, for the process, shared by every file, the REPL and eval.
// Thread-safe.
void addNamespaceAlias(const std::string& alias, const std::string& ns);
// The namespace `alias` stands for, with true; false when it names none.
bool namespaceAlias(const std::string& alias, std::string& ns);
// True for a library namespace protoClojure provides the functions of as
// built-ins: clojure.core, clojure.string, clojure.set, clojure.java.io,
// clojure.java.shell.
bool isKnownNamespace(const std::string& ns);

// True while core.clj loads (Eval.h, loadCore): the current namespace is
// then clojure.core, so a definition is installed under both `x` and
// `clojure.core/x` and syntax-quote qualifies unknown names as
// `clojure.core/x`. Set only on the evaluator thread at start-up.
bool coreNamespaceActive();
void setCoreNamespaceActive(bool active);

// The macro the global `key` holds, or nullptr when it holds none.
const proto::ProtoObject* globalMacro(proto::ProtoContext* ctx,
                                      const ActiveCallContext& cc,
                                      const std::string& key);

// Calls `macro` on the call `form` (a list whose head named it) with the
// environment `env` (nil or a map), as `(macro form env arg...)`, and returns
// the expansion, unrooted (anchored in `ctx`, which the caller keeps alive
// until it has rooted it). A wrong number of arguments raises ArityException
// "Wrong number of args (n) passed to macro: name" before the macro runs,
// `n` counting the arguments the call wrote; whatever the macro raises
// propagates. `form` and `env` must be rooted by the caller.
const proto::ProtoObject* expandMacroCall(proto::ProtoContext* ctx,
                                          const ActiveCallContext& cc,
                                          const proto::ProtoObject* macro,
                                          const std::string& name,
                                          const proto::ProtoObject* form,
                                          const proto::ProtoObject* env);

// One step of macro expansion of `form` with no locals in scope, as
// `macroexpand-1` does: when `form` is a list whose head is a symbol naming a
// macro (and not a special form), the expansion; otherwise `form` itself.
// `form` must be rooted by the caller.
const proto::ProtoObject* macroexpandOnce(proto::ProtoContext* ctx,
                                          const ActiveCallContext& cc,
                                          const proto::ProtoObject* form);

// `eval` lives in the compiler library, which depends on this one; the
// compiler installs its entry point here at start-up (Eval.h).
using EvalHook = const proto::ProtoObject* (*)(proto::ProtoContext* ctx,
                                               const proto::ProtoObject* form);
void setEvalHook(EvalHook hook);
EvalHook evalHook();

} // namespace protoClojure
