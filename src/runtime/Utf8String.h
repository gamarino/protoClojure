/*
 * Utf8String — the protoCore string holding a buffer of UTF-8 bytes.
 *
 * protoCore's zero-terminated constructors (fromUTF8, fromStdString) stop at
 * the first NUL byte, so a string read from `"a\u0000b"` or from a JSON
 * document carrying `\u0000` would be cut there. This builds the string from
 * the whole buffer, embedded NULs included; a trailing incomplete UTF-8
 * sequence becomes U+FFFD, as in the I/O layer (IO.cpp).
 *
 * The result is UNROOTED, like every value a constructor returns.
 */
#pragma once

#include "protoCore.h"

#include <cstdint>
#include <string>

namespace protoClojure {

inline const proto::ProtoObject* makeUtf8String(proto::ProtoContext* ctx,
                                                const std::string& bytes) {
    std::uint8_t rem[4];
    std::uint8_t remCount = 0;
    const proto::ProtoString* s = proto::ProtoString::fromUTF8Buffer(
        ctx, reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(),
        nullptr, 0, rem, &remCount);
    if (remCount == 0) return reinterpret_cast<const proto::ProtoObject*>(s);
    const std::string fixed = bytes.substr(0, bytes.size() - remCount) + "\xEF\xBF\xBD";
    s = proto::ProtoString::fromUTF8Buffer(
        ctx, reinterpret_cast<const std::uint8_t*>(fixed.data()), fixed.size(),
        nullptr, 0, rem, &remCount);
    return reinterpret_cast<const proto::ProtoObject*>(s);
}

// makeUtf8String typed as the ProtoString it is.
inline const proto::ProtoString* makeUtf8ProtoString(proto::ProtoContext* ctx,
                                                     const std::string& bytes) {
    return reinterpret_cast<const proto::ProtoString*>(makeUtf8String(ctx, bytes));
}

} // namespace protoClojure
