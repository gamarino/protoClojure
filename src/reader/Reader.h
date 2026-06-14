/*
 * Reader — turns a token stream into protoCore objects.
 *
 * Architectural rule from day one: every recursive read scope is one
 * `proto::ProtoContext` (== one method invocation in protoCore terms),
 * chained via `previous` to its caller's context, with its own
 * `automaticLocals` slot region holding the in-progress work
 * (the building list, the last-read element). The GC sees every
 * ProtoObject we hold across an allocation; multithreading is free
 * because each ProtoContext is bound to its calling thread.
 *
 * Calling convention for the C++ side:
 *   - Functions receive the *parent* ProtoContext as a parameter.
 *   - A function that has intermediate state across an allocation
 *     pushes its own child ProtoContext at entry and stores that
 *     state in a slot.
 *   - The return value is whatever sits in the relevant slot at the
 *     time of return. The CALLER must store it in its own slot
 *     before the next allocation — returns are unrooted by design.
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
