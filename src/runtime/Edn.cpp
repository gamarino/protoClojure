// Edn — `read-string` and `clojure.edn/read-string` (DataFormats.h,
// docs/DESIGN.md M35): the reader that reads programs, run on a string.
//
// Both read the first form of the string and ignore the rest, as Clojure's
// do. They differ in what they accept:
//   - `read-string` reads as code is read: quote, syntax-quote (qualified
//     the way the drivers qualify it), `#(...)`, `#"..."`;
//   - `clojure.edn/read-string` reads data only: those reader macros are
//     read errors, and there is no evaluation (`#=` does not exist) and no
//     tagged literal (`#inst` raises "No reader function for tag inst").
// A read error is a RuntimeException whose message is the reader's, followed
// by where in the string it happened.

#include "CallerSlots.h"
#include "DataFormats.h"

#include "Exceptions.h"
#include "Macros.h"
#include "MapOps.h"
#include "Named.h"
#include "Primitives.h"

#include "reader/Reader.h"

#include <string>

namespace protoClojure {

namespace {

using PO = proto::ProtoObject;

#define EDNPRIM(name) \
    const PO* name(proto::ProtoContext* ctx, const PO*, const proto::ParentLink*, \
                   const proto::ProtoArguments* args, const proto::ProtoKeywordArguments*)

proto::proto_ulong argc(proto::ProtoContext* ctx, const proto::ProtoArguments* args) {
    return args ? args->getSize(ctx) : 0;
}
const PO* argAt(proto::ProtoContext* ctx, const proto::ProtoArguments* args, proto::proto_ulong i) {
    const PO* v = args->getAt(ctx, static_cast<int>(i));
    return v ? v : PROTO_NONE;
}
[[noreturn]] void arity(proto::ProtoContext* ctx, proto::proto_ulong n, const char* name) {
    throwClassed(ctx, "ArityException",
                 "Wrong number of args (" + std::to_string(n) + ") passed to: " + name);
}

const ActiveCallContext& callContext(proto::ProtoContext* ctx, const char* who) {
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throwError(ctx, std::string(who) + ": no active VM context");
    return *cc;
}

// The text of string argument `v`.
std::string stringArg(proto::ProtoContext* ctx, const PO* v, const char* who) {
    if (!v || v == PROTO_NONE || !proto::ProtoObject::isStringTagFast(v))
        throwClassed(ctx, "ClassCastException",
                     std::string(who) + " expects a string, got " + valueTypeName(ctx, v));
    return reinterpret_cast<const proto::ProtoString*>(v)->toStdString(ctx);
}

// The `:eof` entry of options map `opts`: sets *present.
const PO* eofOption(proto::ProtoContext* ctx, const ActiveCallContext& cc, const PO* opts,
                    const char* who, bool* present) {
    *present = false;
    if (opts == PROTO_NONE) return PROTO_NONE;
    if (!isMap(opts))
        throwClassed(ctx, "ClassCastException",
                     std::string(who) + ": options must be a map, got " + valueTypeName(ctx, opts));
    return mapGet(ctx, opts, internNamed(ctx, cc.named, ":eof"), present);
}

// The first form of `text`, read in code or EDN mode. A text with no form
// answers `eofValue` when `hasEof`, else raises "EOF while reading", as
// Clojure's readers do.
const PO* readFirstForm(proto::ProtoContext* ctx, const ActiveCallContext& cc,
                        const std::string& text, bool edn, bool hasEof, const PO* eofValue) {
    Reader reader(ctx, text, cc.named);
    reader.setEdn(edn);
    if (!edn) {
        if (SyntaxQuoteResolverHook r = syntaxQuoteResolverHook()) reader.setSymbolResolver(r);
    }
    std::string message;
    int line = 0, column = 0;
    try {
        const PO* form = reader.readOne(ctx);
        if (form) return form;
    } catch (const ReaderError& e) {
        message = e.what();
        line = e.line;
        column = e.column;
    }
    if (message.empty()) {
        if (hasEof) return eofValue;
        throwClassed(ctx, "RuntimeException", "EOF while reading");
    }
    throwClassed(ctx, "RuntimeException",
                 message + " (line " + std::to_string(line) + ", column " +
                     std::to_string(column) + ")");
}

// (read-string s), (read-string opts s): the first form of s, read as code.
EDNPRIM(prim_read_string) {
    const proto::proto_ulong n = argc(ctx, args);
    if (n != 1 && n != 2) arity(ctx, n, "read-string");
    const ActiveCallContext& cc = callContext(ctx, "read-string");
    bool hasEof = false;
    const PO* eof = n == 2 ? eofOption(ctx, cc, argAt(ctx, args, 0), "read-string", &hasEof)
                           : PROTO_NONE;
    const std::string text = stringArg(ctx, argAt(ctx, args, n - 1), "read-string");
    return readFirstForm(ctx, cc, text, /*edn=*/false, hasEof, eof);
}

// (clojure.edn/read-string s): nil for nil or a string with no form;
// (clojure.edn/read-string opts s): the :eof option for those, an error
// without one -- clojure.edn's contract.
EDNPRIM(prim_edn_read_string) {
    const proto::proto_ulong n = argc(ctx, args);
    if (n != 1 && n != 2) arity(ctx, n, "clojure.edn/read-string");
    const ActiveCallContext& cc = callContext(ctx, "clojure.edn/read-string");
    bool hasEof = true;
    const PO* eof = PROTO_NONE;
    if (n == 2) eof = eofOption(ctx, cc, argAt(ctx, args, 0), "clojure.edn/read-string", &hasEof);
    const PO* s = argAt(ctx, args, n - 1);
    if (s == PROTO_NONE) return PROTO_NONE;
    const std::string text = stringArg(ctx, s, "clojure.edn/read-string");
    return readFirstForm(ctx, cc, text, /*edn=*/true, hasEof, eof);
}

#undef EDNPRIM

} // namespace

void installEdn(proto::ProtoContext* ctx, proto::ProtoObject* globals) {
    defineBuiltin(ctx, globals, "read-string", ctx->fromMethod(nullptr, &prim_read_string));
    CallerSlots scope(ctx, 1);
    scope.set(0, ctx->space->objectPrototype->newChild(ctx, false));
    scope.set(0, scope.get(0)->setAttribute(
        ctx, proto::ProtoString::createSymbol(ctx, "read-string"),
        ctx->fromMethod(nullptr, &prim_edn_read_string)));
    globals->setAttribute(ctx, proto::ProtoString::createSymbol(ctx, "clojure.edn"),
                          scope.get(0));
}

const char* ednPrimitiveName(proto::ProtoMethod fn) {
    if (fn == &prim_read_string || fn == &prim_edn_read_string) return "read-string";
    return nullptr;
}

} // namespace protoClojure
