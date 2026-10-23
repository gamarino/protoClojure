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
    /* 23 */ {"PatternSyntaxException",          "java.util.regex",        7},
};
constexpr int kExceptionClassCount =
    static_cast<int>(sizeof(kExceptionClasses) / sizeof(kExceptionClasses[0]));
constexpr int kRuntimeException = 3;
constexpr int kExceptionInfo    = 4;

// Set once per ProtoSpace by initExceptions, before any thread other than the
// initialising one runs Clojure code. The prototypes are reachable from the
// globals namespace, which every entry point roots, so the error factory
// reaches them through GC-rooted state.
const proto::ProtoObject* g_classPrototypes[kExceptionClassCount] = {};

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

const proto::ProtoObject* exceptionClassPrototype(int id) {
    return (id >= 0 && id < kExceptionClassCount) ? g_classPrototypes[id] : nullptr;
}

namespace {

// The class protoCore error `kind` arrives as, and its message: JVM
// Clojure's "Divide by zero" for a division by zero, protoCore's own text
// otherwise. A malformed integer literal (Integer::fromString) is a
// NumberFormatException; any other malformed argument an
// IllegalArgumentException (its superclass).
int errorKindClass(proto::ProtoErrorKind kind, const char* message) {
    switch (kind) {
        case proto::ProtoErrorKind::DivisionByZero: return exceptionClassId("ArithmeticException");
        case proto::ProtoErrorKind::Overflow:       return exceptionClassId("ArithmeticException");
        case proto::ProtoErrorKind::TypeMismatch:   return exceptionClassId("ClassCastException");
        case proto::ProtoErrorKind::ArityMismatch:  return exceptionClassId("ArityException");
        case proto::ProtoErrorKind::InvalidArgument:
            return (message && std::strstr(message, "fromString"))
                ? exceptionClassId("NumberFormatException")
                : exceptionClassId("IllegalArgumentException");
    }
    return kRuntimeException;
}

// The space's error factory (ProtoSpace::setErrorFactory): builds the
// Clojure exception the error would be had Clojure code raised it. It must
// not throw; if building the instance fails, it answers the class prototype
// itself, which every catch of the class still selects.
const proto::ProtoObject* clojureErrorFactory(proto::ProtoContext* ctx,
                                              proto::ProtoErrorKind kind,
                                              const char* message) {
    const int cls = errorKindClass(kind, message);
    try {
        const char* text = kind == proto::ProtoErrorKind::DivisionByZero
            ? "Divide by zero" : (message ? message : "");
        // Young cells of the raising context, alive while it is.
        return makeException(ctx, cls, ctx->fromUTF8String(text), nullptr, nullptr);
    } catch (...) {
        return g_classPrototypes[cls];
    }
}

const proto::ProtoString* nameKey(proto::ProtoContext* ctx) {
    return proto::ProtoString::createSymbol(ctx, "name");
}
const proto::ProtoString* foreignMessageKey(proto::ProtoContext* ctx) {
    return proto::ProtoString::createSymbol(ctx, "message");
}

} // namespace

void initExceptions(proto::ProtoContext* ctx, proto::ProtoObject* globals) {
    // One prototype per class; a parent always precedes its children in the
    // table, so each class is created as a child of its finished parent.
    const proto::ProtoList* all = ctx->newList();
    for (int i = 0; i < kExceptionClassCount; ++i) {
        const ExceptionClass& c = kExceptionClasses[i];
        const proto::ProtoObject* parent = c.parent < 0
            ? ctx->space->objectPrototype : g_classPrototypes[c.parent];
        const proto::ProtoObject* cls = parent->newChild(ctx);
        cls = cls->setAttribute(ctx, classKey(ctx), ctx->fromLong(i));
        cls = cls->setAttribute(ctx, nameKey(ctx), ctx->fromUTF8String(c.name));
        g_classPrototypes[i] = cls;
        all = all->appendLast(ctx, cls);
    }
    // Reachable from the globals namespace, which every entry point roots.
    globals->setAttribute(ctx,
        proto::ProtoString::createSymbol(ctx, "__exception_classes__"),
        all->asObject(ctx));
    ctx->space->setErrorFactory(&clojureErrorFactory);
}

bool isException(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
    if (!v || v == PROTO_NONE || !g_classPrototypes[0]) return false;
    // Only object cells have a prototype worth asking for (pointer tag 0).
    if ((reinterpret_cast<std::uintptr_t>(v) & 0x3F) != 0) return false;
    return v->isInstanceOf(ctx, g_classPrototypes[0]) == PROTO_TRUE;
}

bool isForeignObject(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
    if (!v || v == PROTO_NONE) return false;
    if ((reinterpret_cast<std::uintptr_t>(v) & 0x3F) != 0) return false;
    return std::strcmp(valueTypeName(ctx, v), "an object") == 0;
}

bool isThrowable(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
    return isException(ctx, v) || isForeignObject(ctx, v);
}

bool exceptionMatches(proto::ProtoContext* ctx, const proto::ProtoObject* v,
                      int cls) {
    if (cls == kThrowableClassId) return true;           // every value
    if (isException(ctx, v))
        return exceptionClassIsA(exceptionClassOf(ctx, v), cls);
    if (cls == kExceptionClassId) return true;           // a foreign error
    // A foreign value whose chain includes a Clojure class.
    return valueIsInstanceOf(ctx, v, exceptionClassPrototype(cls));
}

bool valueIsInstanceOf(proto::ProtoContext* ctx, const proto::ProtoObject* v,
                       const proto::ProtoObject* type) {
    if (!v || !type || type == PROTO_NONE) return false;
    return v->isInstanceOf(ctx, type) == PROTO_TRUE;
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
    if (cls < 0 || cls >= kExceptionClassCount || !g_classPrototypes[cls])
        throw std::logic_error("makeException: initExceptions was not called");
    // An immutable child of the class prototype: each setAttribute returns a
    // new object with the same parents. The intermediates are young cells of
    // `ctx`, which stays alive for the whole construction.
    const proto::ProtoObject* e = g_classPrototypes[cls]->newChild(ctx);
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

std::string describeThrown(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
    if (isException(ctx, v)) return exceptionToString(ctx, v);
    std::string out;
    if (isForeignObject(ctx, v)) {
        const proto::ProtoObject* name = v->getAttribute(ctx, nameKey(ctx));
        const proto::ProtoObject* message = v->getAttribute(ctx, foreignMessageKey(ctx));
        if (isStringValue(name)) out = stringBytes(ctx, name);
        if (isStringValue(message)) {
            if (!out.empty()) out += ": ";
            out += stringBytes(ctx, message);
        }
        if (!out.empty()) return out;
    }
    printValueTo(ctx, out, v, /*readable=*/true);
    return out;
}

const proto::ProtoObject* thrownMessage(proto::ProtoContext* ctx,
                                        const proto::ProtoObject* v) {
    if (isException(ctx, v)) return exceptionMessage(ctx, v);
    if (isForeignObject(ctx, v)) {
        const proto::ProtoObject* m = v->getAttribute(ctx, foreignMessageKey(ctx));
        if (isStringValue(m)) return m;
    }
    return PROTO_NONE;
}

// ---- Throwing and catching --------------------------------------------------

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
    // context until the ProtoException has pinned the exception; protoCore's
    // pin then keeps it alive after this context is gone.
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(2);
    scope.setAutomaticLocal(0, scope.fromUTF8String(message.c_str()));
    scope.setAutomaticLocal(1, makeException(&scope, cls,
        scope.getAutomaticLocal(0), data, cause));
    ClojureThrow pending(&scope, scope.getAutomaticLocal(1));
    throw pending;
}

namespace {

// Splits "<ClassName>: <message>" into a built-in class and the message;
// RuntimeException and the whole text when the prefix names no class.
int classFromText(const std::string& text, std::string& message) {
    message = text;
    const std::size_t colon = text.find(": ");
    if (colon != std::string::npos && colon > 0) {
        const int named = exceptionClassId(text.substr(0, colon));
        if (named >= 0) {
            message = text.substr(colon + 2);
            return named;
        }
    }
    return kRuntimeException;
}

} // namespace

void throwError(proto::ProtoContext* ctx, const std::string& text) {
    std::string message;
    const int cls = classFromText(text, message);
    throwClassed(ctx, exceptionClassName(cls), message);
}

const proto::ProtoObject* caughtErrorValue(proto::ProtoContext* ctx) {
    try {
        throw;
    } catch (const proto::ProtoException& e) {
        return e.value();
    } catch (const std::exception& e) {
        return exceptionFromError(ctx, e);
    } catch (...) {
        return exceptionFromError(ctx, std::runtime_error("unknown C++ exception"));
    }
}

const proto::ProtoObject* exceptionFromError(proto::ProtoContext* ctx,
                                             const std::exception& error) {
    // "<ClassName>: <message>" names the class when ClassName is a built-in
    // class, simple or qualified. Anything else is a RuntimeException whose
    // message is the whole text.
    std::string message;
    const int cls = classFromText(error.what(), message);
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
                                       const proto::ProtoArguments* args,
                                       const proto::ProtoKeywordArguments*) {
    const proto::proto_ulong n = args ? args->getSize(ctx) : 0;
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
    if (cause != PROTO_NONE && !isThrowable(ctx, cause))
        throwClassed(ctx, "ClassCastException",
            std::string("ex-info expects an exception as the cause, got ") +
            valueTypeName(ctx, cause));
    return makeException(ctx, kExceptionInfo, message, data, cause);
}

namespace {

const proto::ProtoObject* oneArg(proto::ProtoContext* ctx,
                                 const proto::ProtoArguments* args, const char* name) {
    const proto::proto_ulong n = args ? args->getSize(ctx) : 0;
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
                                       const proto::ProtoArguments* args,
                                       const proto::ProtoKeywordArguments*) {
    const proto::ProtoObject* e = oneArg(ctx, args, "ex-data");
    return isException(ctx, e) ? exceptionData(ctx, e) : PROTO_NONE;
}

// (ex-message e): the message of an exception, or the `message` attribute
// of a foreign error object; nil for any other value.
const proto::ProtoObject* prim_ex_message(proto::ProtoContext* ctx,
                                          const proto::ProtoObject*,
                                          const proto::ParentLink*,
                                          const proto::ProtoArguments* args,
                                          const proto::ProtoKeywordArguments*) {
    const proto::ProtoObject* e = oneArg(ctx, args, "ex-message");
    return thrownMessage(ctx, e);
}

// (ex-cause e): the cause of an exception; nil for any other value.
const proto::ProtoObject* prim_ex_cause(proto::ProtoContext* ctx,
                                        const proto::ProtoObject*,
                                        const proto::ParentLink*,
                                        const proto::ProtoArguments* args,
                                        const proto::ProtoKeywordArguments*) {
    const proto::ProtoObject* e = oneArg(ctx, args, "ex-cause");
    return isException(ctx, e) ? exceptionCause(ctx, e) : PROTO_NONE;
}

} // namespace protoClojure
