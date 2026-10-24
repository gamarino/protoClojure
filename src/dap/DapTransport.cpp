#include "dap/DapTransport.h"

#include <cctype>
#include <istream>
#include <ostream>
#include <string>

namespace protoClojure {

namespace {

// Reads one header line, stripping a trailing CRLF or lone LF. Returns false on
// EOF / stream error before any character was read.
bool readHeaderLine(std::istream& in, std::string& line) {
    line.clear();
    int c;
    bool any = false;
    while ((c = in.get()) != std::char_traits<char>::eof()) {
        any = true;
        if (c == '\n') {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            return true;
        }
        line.push_back(static_cast<char>(c));
    }
    // EOF: only treat as a valid (final) line if we collected something.
    return any;
}

// Case-insensitive ASCII comparison for header keys.
bool iequals(const std::string& a, const char* b) {
    size_t i = 0;
    for (; i < a.size() && b[i]; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return i == a.size() && b[i] == '\0';
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

} // namespace

DapTransport::DapTransport(std::istream& in, std::ostream& out)
    : in_(in), out_(out) {}

std::optional<nlohmann::json> DapTransport::readMessage() {
    long long contentLength = -1;
    std::string line;

    // --- header block: lines until a blank line ---
    for (;;) {
        if (!readHeaderLine(in_, line))
            return std::nullopt;          // EOF before any header
        if (line.empty())
            break;                        // blank line ends the header block

        auto colon = line.find(':');
        if (colon == std::string::npos)
            continue;                     // tolerate malformed header lines
        std::string key   = trim(line.substr(0, colon));
        std::string value = trim(line.substr(colon + 1));
        if (iequals(key, "Content-Length")) {
            try {
                contentLength = std::stoll(value);
            } catch (...) {
                return std::nullopt;      // malformed length
            }
        }
    }

    if (contentLength < 0)
        return std::nullopt;              // no Content-Length: cannot frame

    // --- body: exactly contentLength bytes ---
    std::string body(static_cast<size_t>(contentLength), '\0');
    in_.read(body.data(), contentLength);
    if (in_.gcount() != contentLength)
        return std::nullopt;              // truncated / EOF mid-body

    try {
        return nlohmann::json::parse(body);
    } catch (const nlohmann::json::parse_error&) {
        return std::nullopt;              // malformed JSON: never crash
    }
}

void DapTransport::writeMessage(const nlohmann::json& msg) {
    // A script may print bytes that are not UTF-8; replace them rather than
    // throw from dump() and lose the message.
    std::string body = msg.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    std::lock_guard<std::mutex> lock(writeMu_);
    out_ << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    out_.flush();
}

} // namespace protoClojure
