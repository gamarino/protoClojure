#pragma once
#include <nlohmann/json.hpp>
#include <iosfwd>
#include <mutex>
#include <optional>

namespace protoClojure {

// Reads/writes DAP messages (Content-Length framed JSON) over a pair of streams.
// DAP uses the same HTTP-style framing as LSP: a header block terminated by a
// blank line, followed by exactly Content-Length bytes of UTF-8 JSON.
class DapTransport {
public:
    DapTransport(std::istream& in, std::ostream& out);

    // Blocks reading one full DAP message. Returns std::nullopt on EOF, stream
    // error, or malformed input (never throws / crashes on bad input).
    std::optional<nlohmann::json> readMessage();

    // Serializes `msg` and writes it with the Content-Length header. Thread-safe
    // (later tasks send events from a worker thread while the loop reads).
    void writeMessage(const nlohmann::json& msg);

private:
    std::istream& in_;
    std::ostream& out_;
    std::mutex    writeMu_;
};

} // namespace protoClojure
