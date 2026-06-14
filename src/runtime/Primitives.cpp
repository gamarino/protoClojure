#include "Primitives.h"

#include "protoCore.h"

#include <cstdio>

namespace protoClojure {

namespace {

// println — print each positional arg, space-separated, followed by \n.
// Returns nil (PROTO_NONE) per Clojure-JVM semantics.
//
// P1: walks the positional-args ProtoList by index, immediately consuming
// each element. No intermediate ProtoObject* held across an allocation
// that we can avoid. The println'd objects are read-only inspected for
// their string representation.
//
// P3: writes to stdout via std::printf. No std container is stored inside
// protoCore; the std::string we build via toStdString lives transiently
// on the C++ stack.
const proto::ProtoObject* prim_println(proto::ProtoContext* ctx,
                                       const proto::ProtoObject* /*self*/,
                                       const proto::ParentLink* /*parents*/,
                                       const proto::ProtoList* args,
                                       const proto::ProtoSparseList* /*kwargs*/) {
    unsigned long n = args ? args->getSize(ctx) : 0;
    for (unsigned long i = 0; i < n; ++i) {
        if (i > 0) std::fputc(' ', stdout);
        const proto::ProtoObject* a = args->getAt(ctx, static_cast<int>(i));
        if (!a || a == PROTO_NONE) {
            std::fputs("nil", stdout);
            continue;
        }
        if (a->isInteger(ctx)) {
            std::printf("%lld", a->asLong(ctx));
            continue;
        }
        if (a->isString(ctx)) {
            // print the raw characters (no surrounding quotes — println vs
            // prn).
            std::string s = a->asString(ctx)->toStdString(ctx);
            std::fputs(s.c_str(), stdout);
            continue;
        }
        // Anything else: a placeholder for v0.0.x.
        std::fputs("#<unprintable>", stdout);
    }
    std::fputc('\n', stdout);
    std::fflush(stdout);
    return PROTO_NONE;
}

} // namespace

void installPrimitives(proto::ProtoContext* ctx,
                       proto::ProtoObject* globals) {
    // Install each primitive: wrap the C function pointer in a callable
    // ProtoObject via fromMethod, store on the globals under the symbol key.
    // setAttribute on a mutable receiver mutates in place.
    auto install = [&](const char* name, proto::ProtoMethod fn) {
        const proto::ProtoString* key =
            proto::ProtoString::createSymbol(ctx, name);
        const proto::ProtoObject* callable =
            ctx->fromMethod(nullptr /* self */, fn);
        globals->setAttribute(ctx, key, callable);
    };

    install("println", &prim_println);
}

} // namespace protoClojure
