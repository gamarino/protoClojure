/*
 * DapServer — `protoclj --dap`: the Debug Adapter Protocol over standard
 * input and output (docs/debugging.md).
 *
 * The protocol is read on a plain thread of its own, which never touches a
 * ProtoObject. The program runs as `protoclj script.clj` would, on the
 * evaluator thread, compiled with debug information (SourceInfo.h); the
 * server is its DebugHook. A thread that stops stays in the hook, parked in
 * an unmanaged region, and answers the requests that read the program's
 * state (stackTrace, scopes, variables, evaluate) itself, on its own
 * context, between waits. Every thread that runs the program's code (main,
 * futures, pmap, actor workers, HTTP connections) stops on its own: the
 * others run on, and each stopped thread has its own requests, frame ids
 * and resume command. Exception breakpoints (raised, userUnhandled) stop a
 * thread in the hook's onThrow, before the exception propagates.
 */
#pragma once

#include <string>
#include <vector>

namespace protoClojure {

// What the client's `launch` request asks to run.
struct DapLaunch {
    std::string              program;      // the script's path
    std::vector<std::string> args;         // *command-line-args*
    bool                     stopOnEntry = false;
};

// Runs the adapter until the client disconnects. `runProgram` runs the
// launched script, compiled for the debugger, and answers its exit status
// (main.cpp); it is called once, on the calling thread, after
// configurationDone. Answers the exit status.
int runDapServer(int (*runProgram)(const DapLaunch& launch, void* arg), void* arg);

} // namespace protoClojure
