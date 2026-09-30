#include "Exceptions.h"

#include "MapOps.h"
#include "Primitives.h"

#include "protoCore.h"

#include <cstring>

namespace protoClojure {

namespace {

// The built-in hierarchy. Order is the id; a parent always precedes its
// children, and the root has parent -1. `package` is the Java package of the
// class a Clojure programmer knows, so both `ArithmeticException` and
// `java.lang.ArithmeticException` name the same class. Two simplifications
// against Java, recorded in docs/DESIGN.md: SocketTimeoutException hangs
// directly under IOException (Java: via InterruptedIOException), and
// StackOverflowError directly under Error (Java: via VirtualMachineError).
struct ExceptionClass {
    const char* name;
    const char* package;
    int         parent;
};

constexpr ExceptionClass kExceptionClasses[] = {
    /*  0 */ {"Throwable",                       "java.lang",             -1},
    /*  1 */ {"Exception",                       "java.lang",              0},
    /*  2 */ {"Error",                           "java.lang",              0},
    /*  3 */ {"RuntimeException",                "java.lang",              1},
    /*  4 */ {"ExceptionInfo",                   "clojure.lang",           3},
    /*  5 */ {"ArithmeticException",             "java.lang",              3},
    /*  6 */ {"ClassCastException",              "java.lang",              3},
    /*  7 */ {"IllegalArgumentException",        "java.lang",              3},
    /*  8 */ {"ArityException",                  "clojure.lang",           7},
    /*  9 */ {"NumberFormatException",           "java.lang",              7},
    /* 10 */ {"IllegalStateException",           "java.lang",              3},
    /* 11 */ {"IndexOutOfBoundsException",       "java.lang",              3},
    /* 12 */ {"StringIndexOutOfBoundsException", "java.lang",             11},
    /* 13 */ {"NullPointerException",            "java.lang",              3},
    /* 14 */ {"UnsupportedOperationException",   "java.lang",              3},
    /* 15 */ {"IOException",                     "java.io",                1},
    /* 16 */ {"FileNotFoundException",           "java.io",               15},
    /* 17 */ {"SocketException",                 "java.net",              15},
    /* 18 */ {"ConnectException",                "java.net",              17},
    /* 19 */ {"SocketTimeoutException",          "java.net",              15},
    /* 20 */ {"UnknownHostException",            "java.net",              15},
    /* 21 */ {"ExecutionException",              "java.util.concurrent",   1},
    /* 22 */ {"StackOverflowError",              "java.lang",              2},
};
constexpr int kExceptionClassCount =
    static_cast<int>(sizeof(kExceptionClasses) / sizeof(kExceptionClasses[0]));
constexpr int kRuntimeException = 3;
constexpr int kExceptionInfo    = 4;

// Set once per ProtoSpace by initExceptions, before any thread other than the
// initialising one runs Clojure code.
const proto::ProtoObject* g_exceptionMarker = nullptr;
proto::ProtoRootSet*      g_inFlightRoots   = nullptr;

const proto::ProtoString* classKey(proto::ProtoContext* ctx) {
    return proto::ProtoString::createSymbol(ctx, "__ex_class__");
}
const proto::ProtoString* messageKey(proto::ProtoContext* ctx) {
    return proto::ProtoString::createSymbol(ctx, "__ex_message__");
}
const proto::ProtoString* dataKey(proto::ProtoContext* ctx) {
    return proto::ProtoString::createSymbol(ctx, "__ex_data__");
}
const proto::ProtoString* causeKey(proto::ProtoContext* ctx) {
    return proto::ProtoString::createSymbol(ctx, "__ex_cause__");
}

const proto::ProtoObject* orNil(const proto::ProtoObject* v) {
    return v ? v : PROTO_NONE;
}

std::string stringBytes(proto::ProtoContext* ctx, const proto::ProtoObject* s) {
    return reinterpret_cast<const proto::ProtoString*>(s)->toStdString(ctx);
}

bool isStringValue(const proto::ProtoObject* v) {
    return v && v != PROTO_NONE && proto::ProtoObject::isStringTagFast(v);
}

} // namespace

// ---- The class hierarchy ----------------------------------------------------

int exceptionClassId(const std::string& name) {
    for (int i = 0; i < kExceptionClassCount; ++i) {
        const ExceptionClass& c = kExceptionClasses[i];
        if (name == c.name) return i;
        const std::size_t plen = std::strlen(c.package);
        if (name.size() == plen + 1 + std::strlen(c.name) &&
            name.compare(0, plen, c.package) == 0 && name[plen] == '.' &&
            name.compare(plen + 1, std::string::npos, c.name) == 0)
            return i;
    }
    return -1;
}

const char* exceptionClassName(int id) {
    return (id >= 0 && id < kExceptionClassCount) ? kExceptionClasses[id].name
                                                  : "Throwable";
}

bool exceptionClassIsA(int cls, int ancestor) {
    while (cls >= 0 && cls < kExceptionClassCount) {
        if (cls == ancestor) return true;
        cls = kExceptionClasses[cls].parent;
    }
    return false;
}

// ---- Exception values -------------------------------------------------------

void initExceptions(proto::ProtoContext* ctx, proto::ProtoObject* globals) {
    const proto::ProtoObject* marker = ctx->space->objectPrototype->newChild(ctx);
    // Reachable from the globals namespace, which every entry point roots.
    globals->setAttribute(ctx,
        proto::ProtoString::createSymbol(ctx, "__exception_marker__"), marker);
    g_exceptionMarker = marker;
    // One root set per space; protoCore deletes it with the space.
    g_inFlightRoots = ctx->space->createRootSet("protoclj-exceptions-in-flight");
}

bool isException(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
    if (!v || v == PROTO_NONE || !g_exceptionMarker) return false;
    // Only object cells have a prototype worth asking for (pointer tag 0).
    if ((reinterpret_cast<std::uintptr_t>(v) & 0x3F) != 0) return false;
    return v->getPrototype(ctx) == g_exceptionMarker;
}

int exceptionClassOf(proto::ProtoContext* ctx, const proto::ProtoObject* e) {
    const proto::ProtoObject* c = e->getAttribute(ctx, classKey(ctx));
    return (c && c->isInteger(ctx)) ? static_cast<int>(c->asLong(ctx)) : 0;
}

const proto::ProtoObject* exceptionMessage(proto::ProtoContext* ctx,
                                           const proto::ProtoObject* e) {
    return orNil(e->getAttribute(ctx, messageKey(ctx)));
}

const proto::ProtoObject* exceptionData(proto::ProtoContext* ctx,
                                        const proto::ProtoObject* e) {
    return orNil(e->getAttribute(ctx, dataKey(ctx)));
}

const proto::ProtoObject* exceptionCause(proto::ProtoContext* ctx,
                                         const proto::ProtoObject* e) {
    return orNil(e->getAttribute(ctx, causeKey(ctx)));
}

const proto::ProtoObject* makeException(proto::ProtoContext* ctx, int cls,
                                        const proto::ProtoObject* message,
                                        const proto::ProtoObject* data,
                                        const proto::ProtoObject* cause) {
    if (!g_exceptionMarker)
        throw std::logic_error("makeException: initExceptions was not called");
    // An immutable object: each setAttribute returns a new object with the
    // same prototype. The intermediates are young cells of `ctx`, which stays
    // alive for the whole construction.
    const proto::ProtoObject* e = g_exceptionMarker->newChild(ctx);
    e = e->setAttribute(ctx, classKey(ctx), ctx->fromLong(cls));
    e = e->setAttribute(ctx, messageKey(ctx), orNil(message));
    e = e->setAttribute(ctx, dataKey(ctx), orNil(data));
    e = e->setAttribute(ctx, causeKey(ctx), orNil(cause));
    return e;
}

std::string exceptionToString(proto::ProtoContext* ctx,
                              const proto::ProtoObject* e) {
    const int cls = exceptionClassOf(ctx, e);
    std::string out = exceptionClassName(cls);
    const proto::ProtoObject* message = exceptionMessage(ctx, e);
    const proto::ProtoObject* data = exceptionData(ctx, e);
    if (isStringValue(message)) {
        out += ": ";
        out += stringBytes(ctx, message);
    }
    if (data != PROTO_NONE) {
        out += ' ';
        printValueTo(ctx, out, data, /*readable=*/true);
    }
    return out;
}

void appendExceptionForm(proto::ProtoContext* ctx, std::string& out,
                         const proto::ProtoObject* e) {
    out += "#error {:type ";
    out += exceptionClassName(exceptionClassOf(ctx, e));
    out += ", :message ";
    printValueTo(ctx, out, exceptionMessage(ctx, e), /*readable=*/true);
    const proto::ProtoObject* data = exceptionData(ctx, e);
    if (data != PROTO_NONE) {
        out += ", :data ";
        printValueTo(ctx, out, data, /*readable=*/true);
    }
    const proto::ProtoObject* cause = exceptionCause(ctx, e);
    if (cause != PROTO_NONE) {
        out += ", :cause ";
        printValueTo(ctx, out, cause, /*readable=*/true);
    }
    out += '}';
}

// ---- Throwing and catching --------------------------------------------------

// The GC pin of an exception in flight: a handle in the space's root set,
// released when the last copy of the ClojureThrow goes away.
// Not copyable: a copy's destructor would release the handle a second time.
struct ClojureThrow::Pin {
    proto::ProtoRootSet*          roots;
    proto::ProtoRootSet::Handle   handle;
    Pin(proto::ProtoRootSet* rs, const proto::ProtoObject* obj)
        : roots(rs), handle(rs ? rs->add(obj) : proto::ProtoRootSet::kNullHandle) {}
    Pin(const Pin&) = delete;
    Pin& operator=(const Pin&) = delete;
    ~Pin() { if (roots) roots->remove(handle); }
};

ClojureThrow::ClojureThrow(proto::ProtoContext* ctx,
                           const proto::ProtoObject* exception)
    : std::runtime_error(exceptionToString(ctx, exception)),
      value_(exception),
      pin_(std::make_shared<Pin>(g_inFlightRoots, exception)) {}

void throwException(proto::ProtoContext* ctx, const proto::ProtoObject* e) {
    throw ClojureThrow(ctx, e);
}

void throwClassed(proto::ProtoContext* ctx, const char* className,
                  const std::string& message,
                  const proto::ProtoObject* data,
                  const proto::ProtoObject* cause) {
    const int cls = exceptionClassId(className);
    if (cls < 0)
        throw std::logic_error(std::string("throwClassed: unknown class ") +
                               className);
    // The message string and the exception are rooted in a slot of a child
    // context until the ClojureThrow has pinned the exception; the pin then
    // keeps it alive after this context is gone.
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(2);
    scope.setAutomaticLocal(0, scope.fromUTF8String(message.c_str()));
    scope.setAutomaticLocal(1, makeException(&scope, cls,
        scope.getAutomaticLocal(0), data, cause));
    ClojureThrow pending(&scope, scope.getAutomaticLocal(1));
    throw pending;
}

const proto::ProtoObject* exceptionFromError(proto::ProtoContext* ctx,
                                             const std::exception& error) {
    if (const auto* thrown = dynamic_cast<const ClojureThrow*>(&error))
        return thrown->value();

    // "<ClassName>: <message>" names the class when ClassName is a built-in
    // class, simple or qualified. Anything else is a RuntimeException whose
    // message is the whole text.
    const std::string text = error.what();
    int cls = kRuntimeException;
    std::string message = text;
    const std::size_t colon = text.find(": ");
    if (colon != std::string::npos && colon > 0) {
        const int named = exceptionClassId(text.substr(0, colon));
        if (named >= 0) {
            cls = named;
            message = text.substr(colon + 2);
        }
    }
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(1);
    scope.setAutomaticLocal(0, scope.fromUTF8String(message.c_str()));
    // Built in `ctx`, not in `scope`, so the result outlives `scope`: it is
    // a young cell of the caller's live context.
    return makeException(ctx, cls, scope.getAutomaticLocal(0), nullptr, nullptr);
}

// ---- Primitives -------------------------------------------------------------

// (ex-info msg map) / (ex-info msg map cause): an ExceptionInfo. As in JVM
// Clojure, the map may not be nil ("Additional data must be non-nil."), the
// message must be a string or nil, and the cause an exception or nil.
const proto::ProtoObject* prim_ex_info(proto::ProtoContext* ctx,
                                       const proto::ProtoObject*,
                                       const proto::ParentLink*,
                                       const proto::ProtoList* args,
                                       const proto::ProtoSparseList*) {
    const unsigned long n = args ? args->getSize(ctx) : 0;
    if (n != 2 && n != 3)
        throwClassed(ctx, "ArityException",
            "Wrong number of args (" + std::to_string(n) + ") passed to: ex-info");
    const proto::ProtoObject* message = orNil(args->getAt(ctx, 0));
    const proto::ProtoObject* data    = orNil(args->getAt(ctx, 1));
    const proto::ProtoObject* cause   = n == 3 ? orNil(args->getAt(ctx, 2)) : PROTO_NONE;
    if (message != PROTO_NONE && !isStringValue(message))
        throwClassed(ctx, "ClassCastException",
            std::string("ex-info expects a string message, got ") +
            valueTypeName(ctx, message));
    if (data == PROTO_NONE)
        throwClassed(ctx, "IllegalArgumentException",
                     "Additional data must be non-nil.");
    if (!isMap(data))
        throwClassed(ctx, "ClassCastException",
            std::string("ex-info expects a map, got ") + valueTypeName(ctx, data));
    if (cause != PROTO_NONE && !isException(ctx, cause))
        throwClassed(ctx, "ClassCastException",
            std::string("ex-info expects an exception as the cause, got ") +
            valueTypeName(ctx, cause));
    return makeException(ctx, kExceptionInfo, message, data, cause);
}

namespace {

const proto::ProtoObject* oneArg(proto::ProtoContext* ctx,
                                 const proto::ProtoList* args, const char* name) {
    const unsigned long n = args ? args->getSize(ctx) : 0;
    if (n != 1)
        throwClassed(ctx, "ArityException",
            "Wrong number of args (" + std::to_string(n) + ") passed to: " + name);
    return orNil(args->getAt(ctx, 0));
}

} // namespace

// (ex-data e): the data map of an ExceptionInfo; nil for any other value.
const proto::ProtoObject* prim_ex_data(proto::ProtoContext* ctx,
                                       const proto::ProtoObject*,
                                       const proto::ParentLink*,
                                       const proto::ProtoList* args,
                                       const proto::ProtoSparseList*) {
    const proto::ProtoObject* e = oneArg(ctx, args, "ex-data");
    return isException(ctx, e) ? exceptionData(ctx, e) : PROTO_NONE;
}

// (ex-message e): the message of an exception; nil for any other value.
const proto::ProtoObject* prim_ex_message(proto::ProtoContext* ctx,
                                          const proto::ProtoObject*,
                                          const proto::ParentLink*,
                                          const proto::ProtoList* args,
                                          const proto::ProtoSparseList*) {
    const proto::ProtoObject* e = oneArg(ctx, args, "ex-message");
    return isException(ctx, e) ? exceptionMessage(ctx, e) : PROTO_NONE;
}

// (ex-cause e): the cause of an exception; nil for any other value.
const proto::ProtoObject* prim_ex_cause(proto::ProtoContext* ctx,
                                        const proto::ProtoObject*,
                                        const proto::ParentLink*,
                                        const proto::ProtoList* args,
                                        const proto::ProtoSparseList*) {
    const proto::ProtoObject* e = oneArg(ctx, args, "ex-cause");
    return isException(ctx, e) ? exceptionCause(ctx, e) : PROTO_NONE;
}

} // namespace protoClojure
