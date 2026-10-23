#include "reader/Reader.h"
#include "runtime/MapOps.h"
#include "runtime/Named.h"
#include "runtime/VectorOps.h"

#include "protoCore.h"
#include <gtest/gtest.h>

#include <stdexcept>

using protoClojure::NamedLayout;
using protoClojure::Reader;
using protoClojure::ReaderError;

namespace {

// One ProtoSpace per test gives clean isolation. Cheap enough that the test
// binary still finishes in well under a second.
struct ReaderFixture : ::testing::Test {
    proto::ProtoSpace space;
    proto::ProtoContext* ctx = space.rootContext;

    // The intern table of keywords and symbols, as the runtime creates it
    // (src/main.cpp): the reader yields runtime values (Reader.h).
    NamedLayout markers{space.objectPrototype->newChild(ctx),
                        space.objectPrototype->newChild(ctx, /*isMutable=*/true),
                        proto::ProtoString::createSymbol(ctx, "__spelling__")};

    // The text of a string, or the spelling of a symbol or keyword.
    std::string asUtf8(const proto::ProtoObject* form) const {
        if (protoClojure::isNamed(ctx, markers, form))
            return protoClojure::namedSpelling(ctx, markers, form)->toStdString(ctx);
        return reinterpret_cast<const proto::ProtoString*>(form)
            ->toStdString(ctx);
    }

    bool isSymbolNamed(const proto::ProtoObject* form, const char* spelling) const {
        return protoClojure::isNamed(ctx, markers, form) && asUtf8(form) == spelling;
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
    // A string literal is the protoCore string a program builds at run time.
    Reader r(ctx, R"("hello")", markers);
    const proto::ProtoObject* form = r.readOne();
    ASSERT_NE(form, nullptr);
    EXPECT_TRUE(proto::ProtoObject::isStringTagFast(form));
    EXPECT_FALSE(protoClojure::isNamed(ctx, markers, form));
    EXPECT_EQ(asUtf8(form), "hello");
}

TEST_F(ReaderFixture, SymbolIsInterned) {
    // Reading the same symbol text twice produces THE SAME named value, the
    // one `(quote println)` evaluates to.
    Reader r1(ctx, "println", markers);
    Reader r2(ctx, "println", markers);
    const proto::ProtoObject* s1 = r1.readOne();
    const proto::ProtoObject* s2 = r2.readOne();
    ASSERT_NE(s1, nullptr);
    ASSERT_NE(s2, nullptr);
    EXPECT_EQ(s1, s2);
    EXPECT_EQ(s1, protoClojure::internNamed(ctx, markers, "println"));
}

TEST_F(ReaderFixture, KeywordIsANamedValueWithItsColon) {
    Reader r(ctx, ":a :a", markers);
    const proto::ProtoObject* k1 = r.readOne();
    const proto::ProtoObject* k2 = r.readOne();
    EXPECT_TRUE(isSymbolNamed(k1, ":a"));
    EXPECT_EQ(k1, k2);
    EXPECT_FALSE(proto::ProtoObject::isStringTagFast(k1));
}

TEST_F(ReaderFixture, NilTrueFalseAreTheValues) {
    Reader r(ctx, "nil true false nil?", markers);
    EXPECT_EQ(r.readOne(), PROTO_NONE);
    EXPECT_EQ(r.readOne(), PROTO_TRUE);
    EXPECT_EQ(r.readOne(), PROTO_FALSE);
    EXPECT_TRUE(isSymbolNamed(r.readOne(), "nil?"));
}

TEST_F(ReaderFixture, VectorLiteralIsARuntimeVector) {
    Reader r(ctx, "[1 x \"s\"]", markers);
    const proto::ProtoObject* form = r.readOne();
    ASSERT_TRUE(protoClojure::isVector(form));
    const proto::ProtoList* items = protoClojure::vectorItems(ctx, form);
    ASSERT_EQ(items->getSize(ctx), 3u);
    EXPECT_EQ(items->getAt(ctx, 0)->asLong(ctx), 1);
    EXPECT_TRUE(isSymbolNamed(items->getAt(ctx, 1), "x"));
    EXPECT_EQ(asUtf8(items->getAt(ctx, 2)), "s");
}

TEST_F(ReaderFixture, MapLiteralIsARuntimeMap) {
    Reader r(ctx, "{:a 1 \"b\" [2]} {}", markers);
    const proto::ProtoObject* form = r.readOne();
    ASSERT_TRUE(protoClojure::isMap(form));
    EXPECT_EQ(protoClojure::mapCount(ctx, form), 2u);
    bool found = false;
    const proto::ProtoObject* a = protoClojure::mapGet(
        ctx, form, protoClojure::internNamed(ctx, markers, ":a"), &found);
    ASSERT_TRUE(found);
    EXPECT_EQ(a->asLong(ctx), 1);
    const proto::ProtoObject* b =
        protoClojure::mapGet(ctx, form, ctx->fromUTF8String("b"), &found);
    ASSERT_TRUE(found);
    EXPECT_TRUE(protoClojure::isVector(b));
    const proto::ProtoObject* empty = r.readOne();
    ASSERT_TRUE(protoClojure::isMap(empty));
    EXPECT_TRUE(protoClojure::mapIsEmpty(ctx, empty));
}

TEST_F(ReaderFixture, MapLiteralWithARepeatedKeyIsAReadError) {
    Reader r(ctx, "{:a 1 :b 2 :a 3}", markers);
    try {
        r.readOne();
        FAIL() << "a repeated key must be a read error";
    } catch (const ReaderError& e) {
        EXPECT_STREQ(e.what(), "Duplicate key: :a");
    }
}

TEST_F(ReaderFixture, MapLiteralWithAnOddNumberOfFormsIsAReadError) {
    Reader r(ctx, "{:a 1 :b}", markers);
    EXPECT_THROW(r.readOne(), ReaderError);
}

TEST_F(ReaderFixture, DerefIsAListOfTheDerefSymbol) {
    Reader r(ctx, "@a", markers);
    const proto::ProtoList* lst = r.readOne()->asList(ctx);
    ASSERT_EQ(lst->getSize(ctx), 2u);
    EXPECT_TRUE(isSymbolNamed(lst->getAt(ctx, 0), "clojure.core/deref"));
    EXPECT_TRUE(isSymbolNamed(lst->getAt(ctx, 1), "a"));
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
    EXPECT_EQ(head, protoClojure::internNamed(ctx, markers, "println"));

    // The arg is the string "hello".
    EXPECT_TRUE(proto::ProtoObject::isStringTagFast(arg));
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

TEST_F(ReaderFixture, IntegerBeyondLongRangeReadsExactly) {
    Reader r(ctx, "12345678901234567890 -12345678901234567890N", markers);
    const proto::ProtoObject* big = r.readOne();
    ASSERT_NE(big, nullptr);
    ASSERT_TRUE(big->isInteger(ctx));
    EXPECT_EQ(big->asIntegerString(ctx)->toStdString(ctx), "12345678901234567890");
    // protoCore 3.0 raises its Overflow error as a proto::ProtoException.
    EXPECT_THROW(big->asLong(ctx), proto::ProtoException);
    const proto::ProtoObject* negative = r.readOne();
    ASSERT_NE(negative, nullptr);
    EXPECT_EQ(negative->asIntegerString(ctx)->toStdString(ctx), "-12345678901234567890");
}

TEST_F(ReaderFixture, MapLiteralKeysAreRecordedInSourceOrder) {
    // The map has no order; the SourceInfo keeps the literal's (SourceInfo.h).
    protoClojure::SourceInfo info;
    Reader r(ctx, "{:z 1 :a 2 :m {:q 3}}", markers, &info);
    const proto::ProtoObject* form = r.readOne();
    ASSERT_TRUE(protoClojure::isMap(form));
    auto it = info.mapKeyOrder.find(form);
    ASSERT_NE(it, info.mapKeyOrder.end());
    ASSERT_EQ(it->second.size(), 3u);
    EXPECT_TRUE(isSymbolNamed(it->second[0], ":z"));
    EXPECT_TRUE(isSymbolNamed(it->second[1], ":a"));
    EXPECT_TRUE(isSymbolNamed(it->second[2], ":m"));
    EXPECT_EQ(info.mapKeyOrder.size(), 2u);  // the nested map too
}

namespace {

// The form printed back as source, for checking reader expansions: lists in
// parentheses, vectors in brackets, named values by spelling, strings in
// quotes, integers in decimal, nil / true / false by name.
std::string show(proto::ProtoContext* ctx, const NamedLayout& named,
                 const proto::ProtoObject* f) {
    if (f == PROTO_NONE) return "nil";
    if (f == PROTO_TRUE) return "true";
    if (f == PROTO_FALSE) return "false";
    if (protoClojure::isNamed(ctx, named, f))
        return protoClojure::namedSpelling(ctx, named, f)->toStdString(ctx);
    if (proto::ProtoObject::isStringTagFast(f))
        return "\"" + reinterpret_cast<const proto::ProtoString*>(f)->toStdString(ctx) + "\"";
    if (f->isInteger(ctx)) return std::to_string(f->asLong(ctx));
    const bool vec = protoClojure::isVector(f);
    const proto::ProtoList* l = vec ? protoClojure::vectorItems(ctx, f) : f->asList(ctx);
    std::string out = vec ? "[" : "(";
    for (proto::proto_ulong i = 0; i < l->getSize(ctx); ++i) {
        if (i) out += " ";
        out += show(ctx, named, l->getAt(ctx, static_cast<int>(i)));
    }
    return out + (vec ? "]" : ")");
}

} // namespace

TEST_F(ReaderFixture, QuoteUnquoteAndSpliceAreTwoElementLists) {
    Reader r(ctx, "'x '(1 [a]) ~y ~@z foo'", markers);
    EXPECT_EQ(show(ctx, markers, r.readOne()), "(quote x)");
    EXPECT_EQ(show(ctx, markers, r.readOne()), "(quote (1 [a]))");
    EXPECT_EQ(show(ctx, markers, r.readOne()), "(clojure.core/unquote y)");
    EXPECT_EQ(show(ctx, markers, r.readOne()), "(clojure.core/unquote-splicing z)");
    // A `'` inside a symbol is part of it.
    EXPECT_TRUE(isSymbolNamed(r.readOne(), "foo'"));
}

TEST_F(ReaderFixture, SyntaxQuoteExpandsAsClojuresReader) {
    Reader r(ctx, "`a `:k `\"s\" `5 `nil `~x `(f ~x ~@ys) `() `[a ~b] `(a `(b ~c))", markers);
    EXPECT_EQ(show(ctx, markers, r.readOne()), "(quote a)");
    EXPECT_EQ(show(ctx, markers, r.readOne()), ":k");
    EXPECT_EQ(show(ctx, markers, r.readOne()), "\"s\"");
    EXPECT_EQ(show(ctx, markers, r.readOne()), "5");
    EXPECT_EQ(show(ctx, markers, r.readOne()), "nil");
    EXPECT_EQ(show(ctx, markers, r.readOne()), "x");
    EXPECT_EQ(show(ctx, markers, r.readOne()),
              "(clojure.core/seq (clojure.core/concat (clojure.core/list (quote f)) "
              "(clojure.core/list x) ys))");
    EXPECT_EQ(show(ctx, markers, r.readOne()), "(clojure.core/list)");
    EXPECT_EQ(show(ctx, markers, r.readOne()),
              "(clojure.core/apply clojure.core/vector (clojure.core/seq (clojure.core/concat "
              "(clojure.core/list (quote a)) (clojure.core/list b))))");
    // The inner syntax-quote is expanded first; the outer one quotes the
    // expansion as data, so the inner `~c` stays unevaluated: `(quote c)`.
    EXPECT_EQ(show(ctx, markers, r.readOne()),
              "(clojure.core/seq (clojure.core/concat (clojure.core/list (quote a)) "
              "(clojure.core/list (clojure.core/seq (clojure.core/concat "
              "(clojure.core/list (quote clojure.core/seq)) (clojure.core/list "
              "(clojure.core/seq (clojure.core/concat (clojure.core/list (quote clojure.core/concat)) "
              "(clojure.core/list (clojure.core/seq (clojure.core/concat "
              "(clojure.core/list (quote clojure.core/list)) (clojure.core/list "
              "(clojure.core/seq (clojure.core/concat (clojure.core/list (quote quote)) "
              "(clojure.core/list (quote b)))))))) (clojure.core/list (clojure.core/seq "
              "(clojure.core/concat (clojure.core/list (quote clojure.core/list)) "
              "(clojure.core/list (quote c)))))))))))))");
}

TEST_F(ReaderFixture, SyntaxQuoteErrors) {
    {
        Reader r(ctx, "`~@x", markers);
        try { r.readOne(); FAIL() << "a top-level splice must be a read error"; }
        catch (const ReaderError& e) { EXPECT_STREQ(e.what(), "splice (~@) not in list"); }
    }
    {
        Reader r(ctx, "(a ')", markers);
        EXPECT_THROW(r.readOne(), ReaderError);
    }
    {
        Reader r(ctx, "`", markers);
        try { r.readOne(); FAIL() << "a syntax-quote at the end of input must be a read error"; }
        catch (const ReaderError& e) { EXPECT_STREQ(e.what(), "EOF while reading: ` must be followed by a form"); }
    }
}
