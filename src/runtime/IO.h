/*
 * IO — input and output for protoClojure, bound over the protoIO library.
 *
 * The functions are globals named as in Clojure (`slurp`, `spit`,
 * `read-line`, `line-seq`, `reader`, `writer`, `file-seq`, the
 * clojure.java.io helpers `file`, `copy`, `delete-file`, `make-parents`,
 * clojure.java.shell's `sh`), plus babashka http-client's shape for HTTP
 * (`http-get`, `http-post`, ..., `http-request`), Ring's contract for the
 * HTTP server (`run-server`, `stop-server`, `server-port`) and protoClojure's
 * own socket functions (`tcp-connect`, `tcp-listen`, `udp-socket`, ...).
 * docs/LANGUAGE.md §15 is the user-facing reference and docs/DESIGN.md §4.2
 * the architecture.
 *
 * The POSIX layer (buffered descriptors, SIGPIPE, TLS, child processes,
 * sockets, the HTTP message layer and client) lives in protoIO. This file only
 * binds it:
 *   - arguments are copied into C++ values before anything blocks;
 *   - every protoIO call that can block runs inside a
 *     ProtoContext::UnmanagedScope and touches no protoCore object there, so a
 *     thread waiting on I/O never holds up a collection;
 *   - protoCore values are built after the blocking part returns;
 *   - a protoio::Error becomes a catchable exception of the matching class
 *     (FileNotFoundException, ConnectException, SocketTimeoutException,
 *     UnknownHostException, SocketException, IOException,
 *     IllegalArgumentException) whose ex-data is `{:type :file-not-found,
 *     :errno 2}` and the like, raised after the unmanaged scope was left.
 *
 * Handles. Files named with `file`, readers, writers, sockets, listening
 * sockets, UDP sockets and HTTP servers are objects whose prototype is the IO
 * marker created by installIO; they print as `#<reader path>`,
 * `#<socket 127.0.0.1:8080>`, ... A handle holding a descriptor is mutable:
 * `close` swaps its descriptor for -1 with a compare-and-set, so only one
 * thread ever closes the descriptor. A handle is never closed by the
 * collector; `with-open` is the way to scope one.
 */
#pragma once

#include <string>
#include <vector>

namespace proto {
class ProtoContext;
class ProtoObject;
}

namespace protoClojure {

// Creates the IO marker (kept alive as a hidden attribute of `globals`) and
// installs the I/O primitives and `*command-line-args*` (nil) on `globals`.
// Called by installPrimitives.
void installIO(proto::ProtoContext* ctx, proto::ProtoObject* globals);

// Binds `*command-line-args*` to a list of `args`, or nil when there are none,
// as JVM Clojure does for the arguments after the script's path.
void setCommandLineArgs(proto::ProtoContext* ctx, proto::ProtoObject* globals,
                        const std::vector<std::string>& args);

// Stops every HTTP server still running and joins its threads. Call before
// the ProtoSpace is destroyed, and before shutdownFutures and the actor
// scheduler's shutdown (a handler may still be using them). Idempotent.
void shutdownIO(proto::ProtoContext* ctx);

// True when `v` is an I/O handle; appends its printed form (`#<file a/b>`,
// `#<reader in.txt>`, ...) to `out`.
bool appendIOHandle(proto::ProtoContext* ctx, std::string& out,
                    const proto::ProtoObject* v);

// True when `v` is a file handle; appends its path to `out`, which is what
// `str` renders for it (JVM Clojure's File.toString).
bool appendIOFilePath(proto::ProtoContext* ctx, std::string& out,
                      const proto::ProtoObject* v);

// The type name error messages use for an I/O handle ("a file", "a reader",
// "a socket", ...), or nullptr when `v` is not one.
const char* ioHandleTypeName(proto::ProtoContext* ctx, const proto::ProtoObject* v);

// The name an I/O primitive is installed under, or nullptr (for `#<fn NAME>`).
const char* ioPrimitiveName(const void* fn);

} // namespace protoClojure
