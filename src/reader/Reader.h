/*
 * Reader — turns a token stream into protoCore objects.
 *
 * Session 2 scope: reads integers, strings, symbols, and lists. The
 * "code is data" principle goes literal — a list of forms IS a ProtoList of
 * ProtoObjects. No intermediate AST type.
 *
 * Errors are exceptions: ReaderError carries the line/col so the file runner
 * can format a useful message. We use exceptions (rather than return-value
 * propagation) because deep recursive parsing — which a nested list IS —
 * makes return-value plumbing unwieldy.
 */
#pragma once
#include "Lexer.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace proto {
class ProtoContext;
class ProtoObject;
}

namespace protoClojure {

struct ReaderError : std::runtime_error {
    int line   = 0;
    int column = 0;
    ReaderError(const std::string& msg, int ln, int col)
        : std::runtime_error(msg), line(ln), column(col) {}
};

class Reader {
public:
    Reader(proto::ProtoContext* ctx, std::string source);

    // Read one form. Returns nullptr ONLY at EOF. On any other failure
    // throws ReaderError.
    const proto::ProtoObject* readOne();

    // Read every form until EOF, in order.
    std::vector<const proto::ProtoObject*> readAll();

private:
    proto::ProtoContext* ctx_;
    Lexer lexer_;

    const proto::ProtoObject* readFromToken(const Token& tok);
    const proto::ProtoObject* readList(int openLine, int openColumn);
};

} // namespace protoClojure
