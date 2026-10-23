// A Clojure `try` that does not handle an exception rethrows the ORIGINAL
// C++ exception object (protoCore docs/EXCEPTIONS.md, rule E3), not a new
// ProtoException with the same value: a runtime subclass of ProtoException
// keeps its dynamic type and its extra state across Clojure code, and the
// value keeps its identity. Clojure source is read, compiled and run here
// the way the script driver (src/main.cpp) does it.

#include "compiler/Compiler.h"
#include "reader/Reader.h"
#include "runtime/BytecodeModule.h"
#include "runtime/ExecutionEngine.h"
#include "runtime/Exceptions.h"
#include "runtime/Named.h"
#include "runtime/Opcodes.h"
#include "runtime/Primitives.h"

#include "protoCore.h"
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

using namespace protoClojure;

namespace {

// Another runtime's subclass of ProtoException, with state of its own.
struct ForeignThrow : proto::ProtoException {
    ForeignThrow(proto::ProtoContext* ctx, const proto::ProtoObject* v, int extra)
        : proto::ProtoException(ctx, v), traceback(extra) {}
    int traceback;
};

// The value `raise-foreign` throws; rooted as a global of the run.
const proto::ProtoObject* g_foreignValue = nullptr;

// (raise-foreign): throws a ForeignThrow carrying g_foreignValue.
const proto::ProtoObject* primRaiseForeign(proto::ProtoContext* ctx, const proto::ProtoObject*,
                                           const proto::ParentLink*, const proto::ProtoArguments*,
                                           const proto::ProtoKeywordArguments*) {
    throw ForeignThrow(ctx, g_foreignValue, 42);
}

struct RethrowFixture : ::testing::Test {
    proto::ProtoSpace space;
    proto::ProtoContext live{&space, space.rootContext, nullptr, nullptr, nullptr, nullptr};
    std::vector<std::unique_ptr<BytecodeModule>> modules;

    enum Slot { kGlobals, kFnSingle, kFnMulti, kAtom, kFuture,
                kPromise, kActor, kNamed, kNamedTable, kForms, kSlots };

    const proto::ProtoString* sym(const char* s) {
        return proto::ProtoString::createSymbol(&live, s);
    }

    void SetUp() override {
        live.resizeAutomaticLocals(kSlots);
        live.setAutomaticLocal(kGlobals, space.objectPrototype->newChild(&live, true));
        auto* globals = const_cast<proto::ProtoObject*>(live.getAutomaticLocal(kGlobals));
        installPrimitives(&live, globals);
        for (int s = kFnSingle; s <= kNamed; ++s)
            live.setAutomaticLocal(s, space.objectPrototype->newChild(&live));
        live.setAutomaticLocal(kNamedTable, space.objectPrototype->newChild(&live, true));
        // The value the foreign exception carries: an object of the module
        // kind (no Clojure value), kept alive as a global.
        g_foreignValue = space.objectPrototype->newChild(&live)->setAttribute(
            &live, sym("message"), live.fromUTF8String("foreign failure"));
        globals->setAttribute(&live, sym("foreign-value"), g_foreignValue);
        globals->setAttribute(&live, sym("raise-foreign"),
                              live.fromMethod(nullptr, &primRaiseForeign));
    }

    // Reads, compiles and runs `source`; exceptions leave as thrown.
    const proto::ProtoObject* run(const std::string& source) {
        const NamedLayout named{live.getAutomaticLocal(kNamed), live.getAutomaticLocal(kNamedTable),
                                sym("__spelling__")};
        const CompilerMarkers cm{sym("__bytecode__"), sym("__arity__"), sym("__captures__"),
                                 sym("__arities__"), named};
        Reader reader(&live, source, named);
        live.setAutomaticLocal(kForms, reader.readAll()->asObject(&live));
        const proto::ProtoList* forms = live.getAutomaticLocal(kForms)->asList(&live);
        modules.push_back(std::make_unique<BytecodeModule>());
        BytecodeModule& mod = *modules.back();
        Compiler compiler;
        const proto::proto_ulong n = forms->getSize(&live);
        for (proto::proto_ulong i = 0; i < n; ++i) {
            compiler.compileForm(&live, forms->getAt(&live, static_cast<int>(i)), mod, cm);
            if (i + 1 < n) mod.emit(Op::POP, 0);
        }
        mod.emit(Op::RETURN, 0);
        ExecutionEngine eng;
        return eng.run(&live, mod, live.getAutomaticLocal(kGlobals),
                       live.getAutomaticLocal(kFnSingle), live.getAutomaticLocal(kFnMulti),
                       live.getAutomaticLocal(kAtom), live.getAutomaticLocal(kFuture),
                       live.getAutomaticLocal(kPromise), live.getAutomaticLocal(kActor),
                       sym("__bytecode__"), sym("__arity__"), sym("__captures__"),
                       sym("__arities__"), sym("__value__"), sym("__watches__"),
                       sym("__thunk__"), sym("__cc_blob__"), sym("__thread__"),
                       sym("__result__"), sym("__done__"), sym("__actor_state__"),
                       sym("__mailbox__"), named);
    }

    // Runs `source`, which must let raise-foreign's exception escape, and
    // checks the escaping exception is the original ForeignThrow.
    void expectOriginalEscapes(const std::string& source) {
        try {
            run(source);
            FAIL() << "no exception escaped: " << source;
        } catch (const proto::ProtoException& e) {
            const auto* original = dynamic_cast<const ForeignThrow*>(&e);
            ASSERT_NE(original, nullptr) << "the subclass was lost: " << source;
            EXPECT_EQ(original->traceback, 42) << source;
            EXPECT_EQ(e.value(), g_foreignValue) << "the value changed: " << source;
        }
    }
};

}  // namespace

TEST_F(RethrowFixture, NoMatchingClauseRethrowsTheOriginalException) {
    expectOriginalEscapes("(try (raise-foreign) (catch ArithmeticException e :wrong))");
    expectOriginalEscapes(
        "(defn f [] (try (raise-foreign) (catch ArithmeticException e :wrong)"
        "                                (catch IllegalArgumentException e :wrong)))"
        "(f)");
}

TEST_F(RethrowFixture, FinallyRethrowsTheOriginalException) {
    expectOriginalEscapes("(try (raise-foreign) (finally (+ 1 2)))");
    expectOriginalEscapes("(try (raise-foreign) (catch ArithmeticException e :wrong)"
                          "                     (finally (+ 1 2)))");
}

TEST_F(RethrowFixture, NestedTriesAndAHandledInnerExceptionKeepTheOriginal) {
    // An exception handled inside the finally of the outer try, in the same
    // frame, does not disturb the outer rethrow; neither do nested tries.
    expectOriginalEscapes(
        "(try (raise-foreign)"
        "     (finally (try (/ 1 0) (catch ArithmeticException e :handled))))");
    expectOriginalEscapes(
        "(try (try (raise-foreign) (catch ArithmeticException e :wrong))"
        "     (catch ClassCastException e :wrong))");
}

TEST_F(RethrowFixture, AHandlingClauseSeesTheSameValue) {
    const proto::ProtoObject* r = run(
        "(try (try (raise-foreign) (catch ArithmeticException e :wrong))"
        "     (catch Object e (= e foreign-value)))");
    EXPECT_EQ(r, PROTO_TRUE);
}
