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
#include <unordered_map>
#include <vector>

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
//   #_x               nothing: x is read and discarded, as in Clojure
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

    // How syntax-quote qualifies a symbol without a namespace: the resolver
    // answers the spelling to use (`clojure.core/map`, `user/helper`, or the
    // symbol unchanged). The drivers install the one in compiler/Eval.h,
    // which knows the globals; without a resolver symbols stay unqualified.
    using SymbolResolver = std::string (*)(proto::ProtoContext* ctx,
                                           const std::string& name);
    void setSymbolResolver(SymbolResolver r) { resolver_ = r; }

    // EDN mode (`clojure.edn/read-string`, docs/DESIGN.md M35): data only.
    // The code-only reader macros -- `'`, `` ` ``, `~`, `~@`, `@`, `#(...)`
    // and `#"..."` -- are read errors; everything else reads as in code.
    void setEdn(bool edn) { edn_ = edn; }

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
    SymbolResolver resolver_ = nullptr;
    bool edn_ = false;

    // Reads the form after a `#_` token (`tok`) and drops it.
    void discardNextForm(proto::ProtoContext* parent, const Token& tok);

    // One table per syntax-quote being expanded, innermost last: each
    // `foo#` spelling maps to the generated symbol `foo__N__auto__` it
    // stands for throughout that syntax-quote, as in Clojure. The symbols
    // are interned (never collected), so the raw pointers stay valid.
    std::vector<std::unordered_map<std::string, const proto::ProtoObject*>> gensyms_;

    // The `#(...)` literal being read, if any (Clojure forbids nesting
    // them): the generated parameter of each `%N` used so far, by N (`%` is
    // `%1`), and the one `%&` stands for.
    struct FnLiteral {
        std::vector<const proto::ProtoObject*> params;  // index N-1
        const proto::ProtoObject* rest = nullptr;
    };
    FnLiteral* fnLiteral_ = nullptr;

    // The parameter `%`, `%N` or `%&` (`text`) stands for inside the
    // `#(...)` being read, generated on first use; nullptr for any other
    // symbol, or outside a `#(...)`.
    const proto::ProtoObject* fnLiteralArg(proto::ProtoContext* ctx, const std::string& text);

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
    // `what` names the form in the "unterminated" error when it is not
    // the one closeKind implies (a set closes with `}` too).
    const proto::ProtoObject* readList(proto::ProtoContext* parent,
                                       TokenKind closeKind,
                                       int openLine, int openColumn,
                                       const char* what = nullptr);
};

} // namespace protoClojure
