/*
 * Reader — turns a token stream into protoCore objects.
 *
 * Architectural rules — see
 *   docs/archive/design-specs/2026-06-14-engineering-principles.md
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
 */
#pragma once
#include "Lexer.h"
#include "runtime/Named.h"
#include "reader/SourceInfo.h"

#include <stdexcept>
#include <string>

namespace proto {
class ProtoContext;
class ProtoObject;
class ProtoList;
class ProtoString;
}

namespace protoClojure {

struct ReaderError : std::runtime_error {
    int line   = 0;
    int column = 0;
    ReaderError(const std::string& msg, int ln, int col)
        : std::runtime_error(msg), line(ln), column(col) {}
};

// The values the reader yields are the runtime's own values (decision M1,
// docs/DESIGN.md §3 and §10): code is data the program itself can take apart.
//
//   source            value read
//   ------            ----------
//   42, 1.5, 12345N   an integer or a double, as at run time
//   "text"            a protoCore string, as at run time
//   foo, a/b          a symbol: the interned named value (runtime/Named.h)
//   :foo              a keyword: the interned named value spelled ":foo"
//   nil true false    PROTO_NONE, PROTO_TRUE, PROTO_FALSE
//   (a b)             a ProtoList
//   [a b]             a vector (runtime/VectorOps.h)
//   {k v}             a map (runtime/MapOps.h); a repeated key is a read
//                     error, "Duplicate key: k", as in Clojure. The keys'
//                     source order goes to the SourceInfo, if one is given
//                     (SourceInfo.h)
//   @x                the list (clojure.core/deref x)
//   'x                the list (quote x)
//   ~x  ~@x           (clojure.core/unquote x), (clojure.core/unquote-splicing x)
//   `x                the syntax-quote expansion of x (Reader.cpp)
//
// The only thing the reader needs from the runtime is the intern table of
// keywords and symbols.
class Reader {
public:
    // `info`, when not null, receives what the values cannot carry (the
    // source order of map literals); it must outlive the reader's use.
    Reader(proto::ProtoContext* ctx, std::string source,
           const NamedLayout& named, SourceInfo* info = nullptr);

    // Read one form. Returns nullptr ONLY at EOF.
    //
    // The returned pointer is UNROOTED. The caller must store it in a
    // ProtoContext slot (or into a protoCore container that traces it)
    // before any operation that may allocate. This matches the protoCore
    // calling convention.
    const proto::ProtoObject* readOne();

    // readOne, allocating the form in `ctx` instead of the context the
    // reader was built with: a driver reads each top-level form into a
    // context of its own, so the form can be collected once it has run.
    // `ctx` must be the innermost context of the calling thread.
    const proto::ProtoObject* readOne(proto::ProtoContext* ctx);

    // Where the form the last readOne returned starts (1-based), for
    // reporting a compile error at that form.
    int formLine() const { return formLine_; }
    int formColumn() const { return formColumn_; }

    // Replaces the SourceInfo the reader records into (nullptr: none); a
    // driver uses one per top-level form (SourceInfo.h).
    void setSourceInfo(SourceInfo* info) { info_ = info; }

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
    NamedLayout named_;
    SourceInfo* info_;
    int formLine_ = 0;
    int formColumn_ = 0;

    // readAll without the conversion of StackOverflowError to ReaderError.
    const proto::ProtoList* readAllForms();

    // Dispatch on a token to read one form. Receives the parent context
    // so any child context it constructs is correctly chained for the GC.
    const proto::ProtoObject* readFromToken(proto::ProtoContext* parent,
                                            const Token& tok);

    // The form after a reader-macro token (`'`, `@`, ...); EOF is a read
    // error naming the macro.
    const proto::ProtoObject* readNextForm(proto::ProtoContext* parent,
                                           const Token& macroTok);

    // `(head form)`, `form` being the next form read: what `'`, `@`, `~` and
    // `~@` stand for.
    const proto::ProtoObject* readWrapped(proto::ProtoContext* parent,
                                          const Token& macroTok,
                                          const char* head);

    // The expansion of `` `form `` (syntax-quote); see Reader.cpp.
    const proto::ProtoObject* syntaxQuote(proto::ProtoContext* parent,
                                          const proto::ProtoObject* form,
                                          const Token& tok);

    // Read the forms up to the closing token, opening bracket already
    // consumed, as a ProtoList. The caller passes `closeKind` to specify
    // which closing token closes this sequence (RParen for `(...)`,
    // RBracket for `[...]`, RBrace for `{...}`); the caller turns the list
    // into a vector or a map.
    //
    // Pushes its own child ProtoContext; uses slot 0 for the building
    // list and slot 1 for the most recently read element. Returns the
    // slot-0 ProtoObject — caller must root immediately.
    const proto::ProtoObject* readList(proto::ProtoContext* parent,
                                       TokenKind closeKind,
                                       int openLine, int openColumn);
};

} // namespace protoClojure
