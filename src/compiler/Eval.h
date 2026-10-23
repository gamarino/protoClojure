/*
 * Eval — compiling and running one top-level form, shared by the script
 * driver (main.cpp), the REPL (Repl.cpp) and the `eval` built-in.
 *
 * evalTopLevel does what Clojure's Compiler.eval does with a form:
 *
 *   1. expands it while its head names a macro (Compiler::macroexpandTopLevel);
 *   2. if the result is `(do form*)`, evaluates each form in turn the same
 *      way, so a macro that expands into `(do (defmacro m ...) (m))` defines
 *      `m` before the form that uses it is compiled;
 *   3. otherwise compiles the form into a BytecodeModule of its own and runs
 *      it.
 *
 * Every module is kept until releaseRetainedModules: the functions a form
 * defines point into its module, and quoted data are pinned by it
 * (BytecodeModule.h). A module that pins data must die before its
 * ProtoSpace, so each driver releases them, after joining its worker
 * threads, before it returns.
 *
 * Errors: a CompileError (a std::runtime_error, Compiler.h) while expanding
 * or compiling, including a macro that raised (its message names the
 * macro) and a StackOverflowError raised while compiling; a
 * proto::ProtoException, or another C++ exception, while running.
 *
 * The thread must have an ActiveCallContext installed (Primitives.h): it
 * names the globals, the markers and the engine the form runs on, and it is
 * what a macro runs on at compile time.
 */
#pragma once

#include <memory>

namespace proto {
class ProtoContext;
class ProtoObject;
}

namespace protoClojure {

class BytecodeModule;
struct SourceInfo;

// Evaluates the top-level form `form` (rooted by the caller); `info` is what
// the reader recorded about it, or nullptr. The result is unrooted: the
// caller roots it before allocating. `ctx` must be the innermost context of
// the calling thread.
const proto::ProtoObject* evalTopLevel(proto::ProtoContext* ctx,
                                       const proto::ProtoObject* form,
                                       const SourceInfo* info);

// Keeps `mod` alive until releaseRetainedModules. Thread-safe.
void retainModule(std::unique_ptr<BytecodeModule> mod);

// Destroys every retained module. Call once no thread can run code they
// hold, before the ProtoSpace is destroyed.
void releaseRetainedModules();

// Installs the `eval` built-in's entry point (Macros.h): `(eval form)` is
// evalTopLevel, with a compile error raised as a RuntimeException.
void installEval();

} // namespace protoClojure
