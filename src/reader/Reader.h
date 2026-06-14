/*
 * Reader — turns a token stream into protoCore objects.
 *
 * Architectural rules — see
 *   docs/superpowers/specs/2026-06-14-engineering-principles.md
 *
 *   P1: Every ProtoObject* held across an allocation lives in a
 *       ProtoContext::automaticLocals slot. NEVER in a C++ stack
 *       local or member field. The protoCore tracing GC walks
 *       automaticLocals; pointers it cannot see may be reclaimed.
 *
 *   P2: ProtoContext is per method invocation. Each recursive
 *       readList call pushes a child ProtoContext chained to its
 *       parent via `previous`. Multithreading is free because each
 *       context is bound to its calling thread.
 *
 *   P3: No std:: objects inside protoCore. Tokens may carry a
 *       std::string text; on the boundary we use c_str() so
 *       protoCore copies the bytes into its own ProtoString. After
 *       that, the std side is free to die.
 *
 * Session 2 scope: integers, strings, symbols, and lists (the
 * recursive case).
 */
#pragma once
#include "Lexer.h"

#include <stdexcept>
#include <string>

namespace proto {
class ProtoContext;
class ProtoObject;
class ProtoList;
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

    // Read one form. Returns nullptr ONLY at EOF.
    //
    // The returned pointer is UNROOTED. The caller must store it in a
    // ProtoContext slot (or into a protoCore container that traces it)
    // before any operation that may allocate. This matches the protoCore
    // calling convention.
    const proto::ProtoObject* readOne();

    // Read every form until EOF. Returns a protoCore ProtoList of forms in
    // source order — same representation a Clojure file's top-level forms
    // would have. No std container is used; the protoCore GC traces the
    // result and every element it holds.
    //
    // The returned pointer is UNROOTED on return — caller must store
    // immediately, same rule as readOne.
    const proto::ProtoList* readAll();

private:
    proto::ProtoContext* ctx_;
    Lexer lexer_;

    // Dispatch on a token to read one form. Receives the parent context
    // so any child context it constructs is correctly chained for the GC.
    const proto::ProtoObject* readFromToken(proto::ProtoContext* parent,
                                            const Token& tok);

    // Read a list, opening paren already consumed. Pushes its own child
    // ProtoContext; uses slot 0 for the building list and slot 1 for the
    // most recently read element. Returns the slot-0 ProtoObject — caller
    // must root immediately.
    const proto::ProtoObject* readList(proto::ProtoContext* parent,
                                       int openLine, int openColumn);
};

} // namespace protoClojure
