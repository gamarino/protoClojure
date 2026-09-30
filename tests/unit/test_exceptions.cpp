// Exception values (src/runtime/Exceptions.h): the class hierarchy, the
// mapping of C++ error messages onto classes, and the GC pin that keeps an
// exception alive while it is in flight.

#include "runtime/Exceptions.h"
#include "runtime/Primitives.h"

#include "protoCore.h"
#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

using namespace protoClojure;

TEST(ExceptionClasses, SimpleAndQualifiedNamesNameTheSameClass) {
    const char* pairs[][2] = {
        {"Throwable",             "java.lang.Throwable"},
        {"ExceptionInfo",         "clojure.lang.ExceptionInfo"},
        {"ArithmeticException",   "java.lang.ArithmeticException"},
        {"IOException",           "java.io.IOException"},
        {"FileNotFoundException", "java.io.FileNotFoundException"},
        {"ConnectException",      "java.net.ConnectException"},
        {"ExecutionException",    "java.util.concurrent.ExecutionException"},
        {"StackOverflowError",    "java.lang.StackOverflowError"},
    };
    for (const auto& p : pairs) {
        const int id = exceptionClassId(p[0]);
        ASSERT_GE(id, 0) << p[0];
        EXPECT_EQ(exceptionClassId(p[1]), id) << p[1];
        EXPECT_STREQ(exceptionClassName(id), p[0]);
    }
    EXPECT_EQ(exceptionClassId("NoSuchException"), -1);
    EXPECT_EQ(exceptionClassId("java.io.ArithmeticException"), -1);
    EXPECT_EQ(exceptionClassId("lang.ArithmeticException"), -1);
    EXPECT_EQ(exceptionClassId(""), -1);
}

TEST(ExceptionClasses, HierarchyMirrorsJava) {
    auto isA = [](const char* cls, const char* ancestor) {
        return exceptionClassIsA(exceptionClassId(cls), exceptionClassId(ancestor));
    };
    EXPECT_TRUE(isA("ExceptionInfo", "RuntimeException"));
    EXPECT_TRUE(isA("ExceptionInfo", "Exception"));
    EXPECT_TRUE(isA("ExceptionInfo", "Throwable"));
    EXPECT_TRUE(isA("ArityException", "IllegalArgumentException"));
    EXPECT_TRUE(isA("NumberFormatException", "IllegalArgumentException"));
    EXPECT_TRUE(isA("StringIndexOutOfBoundsException", "IndexOutOfBoundsException"));
    EXPECT_TRUE(isA("FileNotFoundException", "IOException"));
    EXPECT_TRUE(isA("ConnectException", "IOException"));
    EXPECT_TRUE(isA("SocketTimeoutException", "IOException"));
    EXPECT_TRUE(isA("UnknownHostException", "IOException"));
    EXPECT_TRUE(isA("StackOverflowError", "Error"));

    // Checked exceptions and errors are not RuntimeExceptions; errors are not
    // Exceptions.
    EXPECT_FALSE(isA("IOException", "RuntimeException"));
    EXPECT_FALSE(isA("ExecutionException", "RuntimeException"));
    EXPECT_FALSE(isA("StackOverflowError", "Exception"));
    EXPECT_FALSE(isA("Exception", "ExceptionInfo"));
    EXPECT_FALSE(isA("ArithmeticException", "ClassCastException"));
}

namespace {

// A space with the exception marker installed, as the drivers set it up.
struct ExceptionsFixture : ::testing::Test {
    proto::ProtoSpace space;
    proto::ProtoContext live{&space, space.rootContext, nullptr, nullptr, nullptr, nullptr};

    void SetUp() override {
        live.resizeAutomaticLocals(2);
        live.setAutomaticLocal(0, space.objectPrototype->newChild(&live, /*isMutable=*/true));
        installPrimitives(&live, const_cast<proto::ProtoObject*>(live.getAutomaticLocal(0)));
    }

    std::string message(const proto::ProtoObject* e) {
        const proto::ProtoObject* m = exceptionMessage(&live, e);
        return reinterpret_cast<const proto::ProtoString*>(m)->toStdString(&live);
    }
};

bool waitForIdleCollector(proto::ProtoSpace& space, proto::ProtoContext* ctx) {
    proto::ProtoContext::UnmanagedScope parked(ctx);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (space.gcStarted.load()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// Runs at least `minCycles` collections, allocating throwaway objects under a
// hard heap limit just above the current heap (protoCore's own pattern, see
// test_value_lifetime.cpp), so freed cells are also recycled.
void forceCollections(proto::ProtoSpace& space, proto::ProtoContext* live,
                      std::uint64_t minCycles) {
    const std::uint64_t start = space.getGCCycleCount();
    space.setHeapLimits(/*soft=*/0, /*hard=*/space.heapSize + 40000);
    for (int batch = 0; batch < 400 && space.getGCCycleCount() - start < minCycles; ++batch) {
        proto::ProtoContext garbage(&space, live, nullptr, nullptr, nullptr, nullptr);
        for (int i = 0; i < 5000; ++i) (void) garbage.newObject(false);
    }
    space.setHeapLimits(0, 0);
    waitForIdleCollector(space, live);
}

}  // namespace

TEST_F(ExceptionsFixture, PrimitiveErrorMessagesMapOntoClasses) {
    const proto::ProtoObject* e = exceptionFromError(
        &live, std::runtime_error("ArithmeticException: Divide by zero"));
    live.setAutomaticLocal(1, e);
    ASSERT_TRUE(isException(&live, e));
    EXPECT_STREQ(exceptionClassName(exceptionClassOf(&live, e)), "ArithmeticException");
    EXPECT_EQ(message(e), "Divide by zero");

    e = exceptionFromError(&live, std::runtime_error(
        "java.io.FileNotFoundException: /nope (No such file or directory)"));
    live.setAutomaticLocal(1, e);
    EXPECT_STREQ(exceptionClassName(exceptionClassOf(&live, e)), "FileNotFoundException");
    EXPECT_EQ(message(e), "/nope (No such file or directory)");

    // No class name: a RuntimeException carrying the whole text.
    for (const char* text : {"inc: expects 1 arg", "VM: unknown opcode",
                             "Foo: bar", ": empty class"}) {
        e = exceptionFromError(&live, std::runtime_error(text));
        live.setAutomaticLocal(1, e);
        EXPECT_STREQ(exceptionClassName(exceptionClassOf(&live, e)), "RuntimeException") << text;
        EXPECT_EQ(message(e), text);
    }
}

TEST_F(ExceptionsFixture, ThrowClassedCarriesClassMessageAndToString) {
    try {
        throwClassed(&live, "java.net.UnknownHostException", "no.such.host");
        FAIL() << "throwClassed returned";
    } catch (const ClojureThrow& t) {
        live.setAutomaticLocal(1, t.value());
        EXPECT_STREQ(t.what(), "UnknownHostException: no.such.host");
        EXPECT_STREQ(exceptionClassName(exceptionClassOf(&live, t.value())),
                     "UnknownHostException");
        EXPECT_EQ(message(t.value()), "no.such.host");
        // exceptionFromError hands back the thrown object itself.
        EXPECT_EQ(exceptionFromError(&live, t), t.value());
    }
    EXPECT_THROW(throwClassed(&live, "NoSuchException", "x"), std::logic_error);
}

// The premise of the pin: an exception whose only owner is a destroyed
// context survives collections while a ClojureThrow for it exists. Built in a
// context that is destroyed before the collections run, exactly as the VM
// frames between a `throw` and its `catch` are. Replacing the root-set add in
// the ClojureThrow constructor with kNullHandle makes this fail: the message
// cell is freed and recycled by the throwaway allocations.
TEST_F(ExceptionsFixture, InFlightExceptionSurvivesCollections) {
    constexpr int kExceptions = 64;
    std::vector<ClojureThrow> inFlight;
    inFlight.reserve(kExceptions);
    for (int i = 0; i < kExceptions; ++i) {
        proto::ProtoContext frame(&space, &live, nullptr, nullptr, nullptr, nullptr);
        frame.resizeAutomaticLocals(2);
        frame.setAutomaticLocal(0, frame.fromUTF8String(
            ("in-flight exception number " + std::to_string(i)).c_str()));
        frame.setAutomaticLocal(1, makeException(&frame, exceptionClassId("IOException"),
            frame.getAutomaticLocal(0), nullptr, nullptr));
        inFlight.emplace_back(&frame, frame.getAutomaticLocal(1));
    }   // each frame is destroyed here and its young generation submitted

    forceCollections(space, &live, 3);

    for (int i = 0; i < kExceptions; ++i) {
        const proto::ProtoObject* e = inFlight[i].value();
        ASSERT_TRUE(isException(&live, e)) << i;
        ASSERT_EQ(exceptionClassOf(&live, e), exceptionClassId("IOException")) << i;
        EXPECT_EQ(message(e), "in-flight exception number " + std::to_string(i));
    }
}
