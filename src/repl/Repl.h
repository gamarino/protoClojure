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
// runRepl(ctx) runs on `ctx`, a context of the evaluator thread (see
// runOnEvaluatorThread), in that context's ProtoSpace, and answers the
// conventional exit code (0 on clean shutdown, non-zero on a fatal startup
// error).
// Worker threads spawned by `(future …)` / `(actor …)` are joined
// before return — same shutdownFutures + ActorScheduler::shutdown
// pattern main.cpp::runFile() uses.

namespace proto {
class ProtoContext;
}

namespace protoClojure {

int runRepl(proto::ProtoContext* ctx);

} // namespace protoClojure
