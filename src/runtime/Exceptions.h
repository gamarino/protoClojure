/*
 * Exceptions — Clojure's exception values and the C++ side of `throw`,
 * `try`, `catch` and `finally`, on protoCore's unified exception model
 * (protoCore docs/EXCEPTIONS.md).
 *
 * Classes. JVM Clojure catches by Java class. protoClojure has no Java
 * classes (deviation D2), so the classes a program can name are a fixed
 * hierarchy mirroring the Java classes a Clojure programmer catches
 * (kExceptionClasses in Exceptions.cpp; docs/DESIGN.md has the tree). Each
 * class is a protoCore prototype object, created by initExceptions: the
 * parent of `ArithmeticException` is `RuntimeException`, whose parent is
 * `Exception`, and so on up to `Throwable`. A class carries two attributes,
 * `__ex_class__` (its index in the table, a SmallInteger) and `name` (its
 * simple name, the attribute other protoCore languages and modules read). A
 * class is named by its simple name (`ArithmeticException`) or by its
 * qualified Java name (`java.lang.ArithmeticException`).
 *
 * Representation. An exception is an immutable child of its class prototype
 * carrying three attributes:
 *
 *   __ex_message__  the message, a string, or nil;
 *   __ex_data__     the data map of an ExceptionInfo (`ex-info`), or nil;
 *   __ex_cause__    the exception that caused this one, or nil.
 *
 * Its class id is inherited from the class prototype, so the class test of a
 * built-in class is an integer walk of at most five parents, and any other
 * type (a module's error prototype) is tested by protoCore's isInstanceOf.
 *
 * Foreign values. A value raised by a module (C++, or another language) is
 * any protoCore value, typically an object whose prototype chain is the
 * module's error hierarchy. `catch` selects it by that chain: a clause names
 * the module's prototype through a var or a module member
 * (`(catch geometry/NegativeRadius e ...)`). `Throwable` and `Object` catch
 * every value, `Exception` every value that is not a Clojure Error (a
 * foreign error is treated as a library's exception). docs/DESIGN.md §4.1.
 *
 * In flight. A thrown value travels up the native stack as a C++ `throw` of
 * a proto::ProtoException (ClojureThrow, a subclass with no extra state, when
 * Clojure code raised it). protoCore pins the value for as long as the
 * exception (or a copy of it) exists; the frame that catches it stores the
 * value in one of its own slots. Every handler catches the base type,
 * `const proto::ProtoException&`, never only ClojureThrow.
 *
 * Errors raised by C++ code. Primitives and the VM raise classed exceptions
 * with throwClassed, or with throwError, which reads the class from a
 * "<Class>: <message>" prefix ("ArithmeticException: Divide by zero"). An
 * error raised by protoCore itself (division by zero, a malformed integer
 * literal, ...) arrives as a ProtoException whose value the error factory
 * installed by initExceptions built (the table is in Exceptions.cpp). A C++
 * exception that is not a ProtoException (`std::exception`, from a module or
 * a library) is a module defect: at a `try` it becomes a RuntimeException
 * carrying `what()` (exceptionFromError), and at the top level the driver
 * reports it as a runtime error.
 */
#pragma once

#include <memory>
#include <stdexcept>
#include <string>

#include "protoCore.h"   // proto::ProtoException

namespace protoClojure {

// ---- The class hierarchy ----------------------------------------------------

// The id of the class named `name`, simple (`IOException`) or qualified
// (`java.io.IOException`); -1 when no built-in class has that name.
int exceptionClassId(const std::string& name);

// The simple name of class `id` (`"ExceptionInfo"`).
const char* exceptionClassName(int id);

// True when class `cls` is `ancestor` or a subclass of it.
bool exceptionClassIsA(int cls, int ancestor);

// The ids of the two classes with a rule of their own in exceptionMatches.
constexpr int kThrowableClassId = 0;
constexpr int kExceptionClassId = 1;

// The prototype object of built-in class `id` (initExceptions creates them).
const proto::ProtoObject* exceptionClassPrototype(int id);

// ---- Exception values -------------------------------------------------------

// Creates the class prototypes for the ProtoSpace of `ctx`, keeps them alive
// as a hidden attribute of `globals`, and installs the space's error factory
// (protoCore errors as Clojure exceptions). Called by installPrimitives,
// before any other thread runs code in the space.
void initExceptions(proto::ProtoContext* ctx, proto::ProtoObject* globals);

// True when `v` is a Clojure exception value: an instance of Throwable.
bool isException(proto::ProtoContext* ctx, const proto::ProtoObject* v);

// True when `v` is a protoCore object that is no Clojure value (no
// collection, fn, atom, exception, ...): what a module typically raises.
bool isForeignObject(proto::ProtoContext* ctx, const proto::ProtoObject* v);

// True when `throw` accepts `v`: a Clojure exception or a foreign object.
bool isThrowable(proto::ProtoContext* ctx, const proto::ProtoObject* v);

// True when a `catch` of built-in class `cls` handles the thrown value `v`
// (see the file comment for Throwable / Object and Exception).
bool exceptionMatches(proto::ProtoContext* ctx, const proto::ProtoObject* v,
                      int cls);

// True when a `catch` of the type `type` (a prototype object resolved at
// run time) handles the thrown value `v`: `type` is on v's prototype chain.
bool valueIsInstanceOf(proto::ProtoContext* ctx, const proto::ProtoObject* v,
                       const proto::ProtoObject* type);

// The text of any thrown value, as the top-level report and the message of
// an ExecutionException show it: a Clojure exception's toString; for a
// foreign object, its `name` attribute and its `message` attribute
// ("NegativeRadius: circleArea: ..."); else its printed form.
std::string describeThrown(proto::ProtoContext* ctx, const proto::ProtoObject* v);

// The message of a thrown value, as `ex-message` answers it: a Clojure
// exception's message, a foreign object's `message` attribute when it is a
// string; nil otherwise.
const proto::ProtoObject* thrownMessage(proto::ProtoContext* ctx,
                                        const proto::ProtoObject* v);

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

// A value thrown by Clojure code (or by the runtime's C++ on its behalf).
// It adds no state to proto::ProtoException: protoCore pins the value while
// any copy is in flight, and every handler catches the base type. It does
// not derive from std::exception: a language value is not a C++ failure.
class ClojureThrow : public proto::ProtoException {
public:
    // `value` must be rooted by the caller at the moment of the call.
    ClojureThrow(proto::ProtoContext* ctx, const proto::ProtoObject* value)
        : proto::ProtoException(ctx, value) {}
};

// Throws the value `e` (rooted by the caller) as a ClojureThrow.
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

// Throws a classed exception from a message in the "<Class>: <message>"
// form; a message whose prefix names no built-in class raises a
// RuntimeException carrying the whole text (deviation D26). The helper for
// the runtime's older string-shaped errors.
[[noreturn]] void throwError(proto::ProtoContext* ctx, const std::string& text);

// The exception value for a C++ exception that is not a ProtoException (a
// module defect, or a StackOverflowError): a new exception of the class its
// message starts with, else a RuntimeException carrying `what()`. The result
// is not rooted (a young cell of `ctx`): the caller stores it in a slot
// before anything else allocates.
const proto::ProtoObject* exceptionFromError(proto::ProtoContext* ctx,
                                             const std::exception& error);

// The value of the exception being handled. Call only inside a catch block:
// a ProtoException answers its value (pinned while the handler runs), any
// other C++ exception the result of exceptionFromError. The caller roots the
// result before the handler ends.
const proto::ProtoObject* caughtErrorValue(proto::ProtoContext* ctx);

// ---- Primitives -------------------------------------------------------------

const proto::ProtoObject* prim_ex_info(proto::ProtoContext*, const proto::ProtoObject*,
                                       const proto::ParentLink*, const proto::ProtoArguments*,
                                       const proto::ProtoKeywordArguments*);
const proto::ProtoObject* prim_ex_data(proto::ProtoContext*, const proto::ProtoObject*,
                                       const proto::ParentLink*, const proto::ProtoArguments*,
                                       const proto::ProtoKeywordArguments*);
const proto::ProtoObject* prim_ex_message(proto::ProtoContext*, const proto::ProtoObject*,
                                          const proto::ParentLink*, const proto::ProtoArguments*,
                                          const proto::ProtoKeywordArguments*);
const proto::ProtoObject* prim_ex_cause(proto::ProtoContext*, const proto::ProtoObject*,
                                        const proto::ParentLink*, const proto::ProtoArguments*,
                                        const proto::ProtoKeywordArguments*);

} // namespace protoClojure
