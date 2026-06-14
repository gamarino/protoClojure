#include "reader/Reader.h"

#include "protoCore.h"
#include <gtest/gtest.h>

using protoClojure::Reader;
using protoClojure::ReaderError;
using protoClojure::ReaderMarkers;

namespace {

// One ProtoSpace per test gives clean isolation. Cheap enough that the test
// binary still finishes in well under a second.
struct ReaderFixture : ::testing::Test {
    proto::ProtoSpace space;
    proto::ProtoContext* ctx = space.rootContext;

    const proto::ProtoObject* stringMarker =
        space.objectPrototype->newChild(ctx, /*isMutable=*/true);
    const proto::ProtoString* bytesKey =
        proto::ProtoString::createSymbol(ctx, "__bytes__");
    ReaderMarkers markers{stringMarker, bytesKey};

    // Convenience: extract the bytes of either a bare ProtoString (symbol or
    // string-tagged) or a string-marker-wrapped object.
    std::string asUtf8(const proto::ProtoObject* form) const {
        if (form && form->getPrototype(ctx) == stringMarker) {
            form = form->getAttribute(ctx, bytesKey);
        }
        return reinterpret_cast<const proto::ProtoString*>(form)
            ->toStdString(ctx);
    }
};

} // namespace

TEST_F(ReaderFixture, IntegerLiteral) {
    Reader r(ctx, "42", markers);
    const proto::ProtoObject* form = r.readOne();
    ASSERT_NE(form, nullptr);
    EXPECT_TRUE(form->isInteger(ctx));
    EXPECT_EQ(form->asLong(ctx), 42);
}

TEST_F(ReaderFixture, NegativeInteger) {
    Reader r(ctx, "-7", markers);
    const proto::ProtoObject* form = r.readOne();
    ASSERT_NE(form, nullptr);
    EXPECT_EQ(form->asLong(ctx), -7);
}

TEST_F(ReaderFixture, StringLiteral) {
    // String tokens come back wrapped (see ReaderMarkers doc) — the asUtf8
    // fixture helper unwraps via the bytesKey attribute.
    Reader r(ctx, R"("hello")", markers);
    const proto::ProtoObject* form = r.readOne();
    ASSERT_NE(form, nullptr);
    EXPECT_EQ(form->getPrototype(ctx), stringMarker);
    EXPECT_EQ(asUtf8(form), "hello");
}

TEST_F(ReaderFixture, SymbolIsInterned) {
    // Reading the same symbol text twice produces THE SAME pointer.
    Reader r1(ctx, "println", markers);
    Reader r2(ctx, "println", markers);
    const proto::ProtoObject* s1 = r1.readOne();
    const proto::ProtoObject* s2 = r2.readOne();
    ASSERT_NE(s1, nullptr);
    ASSERT_NE(s2, nullptr);
    EXPECT_EQ(s1, s2) << "createSymbol must return the same pointer for "
                         "identical names within a ProtoSpace";
}

TEST_F(ReaderFixture, SymbolVsStringDistinctIdentity) {
    Reader r1(ctx, "println", markers);
    Reader r2(ctx, R"("println")", markers);
    const proto::ProtoObject* sym = r1.readOne();
    const proto::ProtoObject* str = r2.readOne();
    EXPECT_NE(sym, str)
        << "an interned symbol and a heap string must have different identity";
}

TEST_F(ReaderFixture, EmptyList) {
    Reader r(ctx, "()", markers);
    const proto::ProtoObject* form = r.readOne();
    ASSERT_NE(form, nullptr);
    const proto::ProtoList* lst = form->asList(ctx);
    ASSERT_NE(lst, nullptr);
    EXPECT_EQ(lst->getSize(ctx), 0u);
}

TEST_F(ReaderFixture, HelloWorldShape) {
    // The shape we need for v0.1 Week 1 — (println "hello").
    Reader r(ctx, R"((println "hello"))", markers);
    const proto::ProtoObject* form = r.readOne();
    ASSERT_NE(form, nullptr);

    const proto::ProtoList* lst = form->asList(ctx);
    ASSERT_NE(lst, nullptr);
    EXPECT_EQ(lst->getSize(ctx), 2u);

    const proto::ProtoObject* head = lst->getAt(ctx, 0);
    const proto::ProtoObject* arg  = lst->getAt(ctx, 1);
    ASSERT_NE(head, nullptr);
    ASSERT_NE(arg, nullptr);

    // The head is the interned `println` symbol.
    const proto::ProtoString* printlnSym =
        proto::ProtoString::createSymbol(ctx, "println");
    EXPECT_EQ(head, reinterpret_cast<const proto::ProtoObject*>(printlnSym));

    // The arg is the wrapped string "hello".
    EXPECT_EQ(arg->getPrototype(ctx), stringMarker);
    EXPECT_EQ(asUtf8(arg), "hello");
}

TEST_F(ReaderFixture, NestedLists) {
    Reader r(ctx, "(a (b c) d)", markers);
    const proto::ProtoObject* form = r.readOne();
    ASSERT_NE(form, nullptr);
    const proto::ProtoList* lst = form->asList(ctx);
    ASSERT_NE(lst, nullptr);
    EXPECT_EQ(lst->getSize(ctx), 3u);

    const proto::ProtoObject* mid = lst->getAt(ctx, 1);
    const proto::ProtoList* inner = mid->asList(ctx);
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(inner->getSize(ctx), 2u);
}

TEST_F(ReaderFixture, MultipleTopLevelForms) {
    Reader r(ctx, "1 2 3", markers);
    const proto::ProtoList* forms = r.readAll();
    ASSERT_NE(forms, nullptr);
    EXPECT_EQ(forms->getSize(ctx), 3u);
    EXPECT_EQ(forms->getAt(ctx, 0)->asLong(ctx), 1);
    EXPECT_EQ(forms->getAt(ctx, 1)->asLong(ctx), 2);
    EXPECT_EQ(forms->getAt(ctx, 2)->asLong(ctx), 3);
}

TEST_F(ReaderFixture, EOFReturnsNullptr) {
    Reader r(ctx, "", markers);
    EXPECT_EQ(r.readOne(), nullptr);
}

TEST_F(ReaderFixture, UnmatchedOpenIsError) {
    Reader r(ctx, "(a b c", markers);
    EXPECT_THROW(r.readOne(), ReaderError);
}

TEST_F(ReaderFixture, UnmatchedCloseIsError) {
    Reader r(ctx, ")", markers);
    EXPECT_THROW(r.readOne(), ReaderError);
}

TEST_F(ReaderFixture, MalformedNumberIsError) {
    Reader r(ctx, "42abc", markers);
    EXPECT_THROW(r.readOne(), ReaderError);
}
