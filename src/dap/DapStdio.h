#pragma once
#include <functional>
#include <istream>
#include <memory>
#include <ostream>
#include <string>

namespace proto { class ProtoContext; }

namespace protoClojure {

// Keeps the DAP channel apart from the debuggee's standard streams.
//
// `protoclj --dap` speaks the protocol over its stdin/stdout, and the debuggee's
// println / print / prn write to fd 1 as well: interleaved with the
// protocol they break its Content-Length framing, and the client drops the
// session at the first line the script prints. On construction the protocol
// takes private duplicates of fds 0 and 1; fds 1 and 2 are pointed at pipes
// whose bytes startForwarding() hands to a sink (the server turns them into
// `output` events), and fd 0 at the null device, so a script reading stdin
// sees end of file instead of eating DAP messages.
//
// If the descriptors cannot be set up, nothing is redirected and the protocol
// runs over std::cin / std::cout as before.
class DapStdio {
public:
    // Called from a forwarding thread with "stdout" or "stderr" and a chunk of
    // text that never ends inside a UTF-8 sequence.
    using Sink = std::function<void(const char* category, const std::string& text)>;

    DapStdio();
    ~DapStdio();
    DapStdio(const DapStdio&) = delete;
    DapStdio& operator=(const DapStdio&) = delete;

    std::istream& protocolIn();
    std::ostream& protocolOut();

    // Starts the threads that drain the pipes into `sink`. Call once, after
    // whatever `sink` writes to exists.
    void startForwarding(Sink sink);

    // Waits (at most about a second) until what the program wrote to fds 1
    // and 2 so far went through the sink, so an event sent next follows the
    // output it should follow. The caller flushes the stdio buffers first;
    // a thread registered with protoCore calls it in an unmanaged region.
    void waitForwarded();

    // Flushes stdout / stderr, stops capturing them and waits until every byte
    // written so far went through the sink. Later writes to fd 1 are dropped;
    // fd 2 goes back to the adapter's own stderr. `ctx` is the calling
    // thread's context when it is registered with protoCore (nullptr if not):
    // the wait runs in an unmanaged region on it. Idempotent.
    void finish(proto::ProtoContext* ctx);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace protoClojure
