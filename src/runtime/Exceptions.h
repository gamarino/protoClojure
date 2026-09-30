/*
 * Exceptions — Clojure's exception values and the C++ side of `throw`,
 * `try`, `catch` and `finally`.
 *
 * Representation. An exception is an immutable protoCore object whose
 * prototype is the process's exception marker (created by initExceptions).
 * It carries four attributes:
 *
 *   __ex_class__    the class, as a SmallInteger id into the built-in
 *                   class table below (never a string, so a class test is
 *                   an integer compare and a walk up at most five parents);
 *   __ex_message__  the message, a string, or nil;
 *   __ex_data__     the data map of an ExceptionInfo (`ex-info`), or nil;
 *   __ex_cause__    the exception that caused this one, or nil.
 *
 * Classes. JVM Clojure catches by Java class. protoClojure has no Java
 * classes (deviation D2), so the classes a program can name are a small
 * fixed hierarchy mirroring the Java classes a Clojure programmer catches
 * (kExceptionClasses in Exceptions.cpp; docs/DESIGN.md has the tree). A
 * class is named by its simple name (`ArithmeticException`) or by its
 * qualified Java name (`java.lang.ArithmeticException`,
 * `clojure.lang.ExceptionInfo`).
 *
 * In flight. A thrown exception travels up the native stack as a C++
 * `ClojureThrow`. The VM frames it unwinds through are destroyed on the way,
 * and with them the contexts whose young generations may be the only thing
 * keeping the exception object alive, so a ClojureThrow pins its object in a
 * protoCore ProtoRootSet for as long as it (or a copy of it) exists. The
 * frame that catches it stores the object in one of its own slots before
 * the ClojureThrow is destroyed.
 *
 * Errors raised by C++ code. Primitives, the VM and the stack guard raise
 * std::runtime_error whose message starts with the class name
 * ("ArithmeticException: Divide by zero"). When such an error reaches a
 * `try`, exceptionFromError turns it into an exception of that class with
 * the rest of the text as its message; an error whose message names no
 * known class becomes a RuntimeException carrying the whole message
 * (deviation D26). New C++ code raises exceptions with throwClassed, which
 * can also attach a data map and a cause.
 */
#pragma once

#include <memory>
#include <stdexcept>
#include <string>

namespace proto {
class ProtoContext;
class ProtoObject;
class ProtoList;
class ProtoSparseList;
class ParentLink;
}

namespace protoClojure {

// ---- The class hierarchy ----------------------------------------------------

// The id of the class named `name`, simple (`IOException`) or qualified
// (`java.io.IOException`); -1 when no built-in class has that name.
int exceptionClassId(const std::string& name);

// The simple name of class `id` (`"ExceptionInfo"`).
const char* exceptionClassName(int id);

// True when class `cls` is `ancestor` or a subclass of it.
bool exceptionClassIsA(int cls, int ancestor);

// ---- Exception values -------------------------------------------------------

// Creates the exception marker and the root set that pins exceptions in
// flight, for the ProtoSpace of `ctx`; the marker is kept alive as a hidden
// attribute of `globals`. Called by installPrimitives.
void initExceptions(proto::ProtoContext* ctx, proto::ProtoObject* globals);

// True when `v` is an exception value.
bool isException(proto::ProtoContext* ctx, const proto::ProtoObject* v);

// The parts of exception `e` (which must be an exception): its class id,
// and its message, data and cause (PROTO_NONE when absent).
int exceptionClassOf(proto::ProtoContext* ctx, const proto::ProtoObject* e);
const proto::ProtoObject* exceptionMessage(proto::ProtoContext* ctx,
                                           const proto::ProtoObject* e);
const proto::ProtoObject* exceptionData(proto::ProtoContext* ctx,
                                        const proto::ProtoObject* e);
const proto::ProtoObject* exceptionCause(proto::ProtoContext* ctx,
                                         const proto::ProtoObject* e);

// A new exception of class `cls`. `message` is a string or nil, `data` a map
// or nil, `cause` an exception or nil; nullptr means nil. The arguments must
// be rooted by the caller; the result is a young cell of `ctx`.
const proto::ProtoObject* makeException(proto::ProtoContext* ctx, int cls,
                                        const proto::ProtoObject* message,
                                        const proto::ProtoObject* data,
                                        const proto::ProtoObject* cause);

// The exception's toString, as `str` renders it and the top-level error
// report shows it: "<class>: <message>", followed for an ExceptionInfo by a
// space and its data map (`ExceptionInfo: boom {:a 1}`); just "<class>" when
// the message is nil and there is no data.
std::string exceptionToString(proto::ProtoContext* ctx,
                              const proto::ProtoObject* e);

// Appends the printed form of exception `e` to `out`: a one-line
// `#error {:type C, :message "m", :data {...}, :cause #error {...}}`, where
// :data appears when the exception has data and :cause when it has a cause
// (deviation D27).
void appendExceptionForm(proto::ProtoContext* ctx, std::string& out,
                         const proto::ProtoObject* e);

// ---- Throwing and catching --------------------------------------------------

// A Clojure exception in flight on the native stack. `what()` is the
// exception's toString, so every existing `catch (const std::exception&)` —
// the script driver, the REPL — reports it as it reports any runtime error.
// The exception object is pinned as a GC root for as long as any copy of the
// ClojureThrow exists (see the file comment).
class ClojureThrow : public std::runtime_error {
public:
    // `exception` must be rooted by the caller at the moment of the call.
    ClojureThrow(proto::ProtoContext* ctx, const proto::ProtoObject* exception);
    const proto::ProtoObject* value() const { return value_; }

private:
    struct Pin;
    const proto::ProtoObject* value_;
    std::shared_ptr<Pin> pin_;
};

// Throws the exception `e` (rooted by the caller).
[[noreturn]] void throwException(proto::ProtoContext* ctx,
                                 const proto::ProtoObject* e);

// Throws a new exception of the class named `className` (simple or
// qualified; an unknown name throws std::logic_error, a programming error)
// with `message`, an optional data map and an optional cause. `data` and
// `cause` must be rooted by the caller. The helper C++ code uses to raise a
// catchable, classed error:
//
//     throwClassed(ctx, "FileNotFoundException", path + " (No such file)");
//     throwClassed(ctx, "ExceptionInfo", "request failed", dataMap);
[[noreturn]] void throwClassed(proto::ProtoContext* ctx, const char* className,
                               const std::string& message,
                               const proto::ProtoObject* data = nullptr,
                               const proto::ProtoObject* cause = nullptr);

// The exception value for an error caught in C++: the object of a
// ClojureThrow, or, for any other std::exception, a new exception of the
// class its message starts with (see the file comment). The result is not
// rooted (a new one is a young cell of `ctx`): the caller stores it in a
// slot before anything else allocates.
const proto::ProtoObject* exceptionFromError(proto::ProtoContext* ctx,
                                             const std::exception& error);

// ---- Primitives -------------------------------------------------------------

const proto::ProtoObject* prim_ex_info(proto::ProtoContext*, const proto::ProtoObject*,
                                       const proto::ParentLink*, const proto::ProtoList*,
                                       const proto::ProtoSparseList*);
const proto::ProtoObject* prim_ex_data(proto::ProtoContext*, const proto::ProtoObject*,
                                       const proto::ParentLink*, const proto::ProtoList*,
                                       const proto::ProtoSparseList*);
const proto::ProtoObject* prim_ex_message(proto::ProtoContext*, const proto::ProtoObject*,
                                          const proto::ParentLink*, const proto::ProtoList*,
                                          const proto::ProtoSparseList*);
const proto::ProtoObject* prim_ex_cause(proto::ProtoContext*, const proto::ProtoObject*,
                                        const proto::ParentLink*, const proto::ProtoList*,
                                        const proto::ProtoSparseList*);

} // namespace protoClojure
