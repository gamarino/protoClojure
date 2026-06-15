#pragma once

// protoClojure interactive REPL.
//
// Surface a Clojure programmer expects: prompt `user=>`, continuation
// prompt `  #_=>`, multi-line forms held until balanced delimiters,
// libreadline history persisted to ~/.protoclj_history, `*1` / `*2` /
// `*3` bindings to the last three evaluation results, error recovery
// that does not crash the loop, and `:help` / `:quit` / `:load` /
// `:time` meta-commands.
//
// runRepl() owns its own ProtoSpace and exits with the conventional
// exit code (0 on clean shutdown, non-zero on a fatal startup error).
// Worker threads spawned by `(future …)` / `(actor …)` are joined
// before return — same shutdownFutures + ActorScheduler::shutdown
// pattern main.cpp::runFile() uses.

namespace protoClojure {

int runRepl();

} // namespace protoClojure
