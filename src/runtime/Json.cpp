// Json — one JSON parser and one JSON writer behind the two APIs Clojure
// scripts use (DataFormats.h, docs/DESIGN.md M36-M38):
//
//   cheshire.core      (parse-string s [key-fn]) (parse-stream rdr [key-fn])
//                      (generate-string x [opts]) (generate-stream x w [opts])
//                      decode / encode (the same functions)
//   clojure.data.json  (read-str s & opts) (read rdr & opts)
//                      (write-str x & opts) (write x w & opts)
//
// The two APIs differ only where the libraries differ (a Flavor): how a
// keyword is written (cheshire keeps its namespace, data.json writes its
// name), what is escaped by default, how NaN is written, what happens after
// the first value, and which exception an error raises.
//
// Parsing (RFC 8259):
//   - objects become maps, arrays vectors, strings strings; `true`, `false`
//     and `null` are true, false and nil;
//   - a number without a fraction or an exponent is an integer, exact at any
//     size (a LargeInteger beyond 64 bits); any other is a double;
//   - escapes: \" \\ \/ \b \f \n \r \t and \uXXXX, a surrogate pair being
//     the one character it encodes; an unpaired surrogate is an error (a
//     UTF-8 string cannot hold one, D57);
//   - an error is raised with the line and the column (in characters) where
//     the input stopped making sense.
// The parser recurses once per level of nesting; a document nested deeper
// than the native stack allows raises StackOverflowError, as the reader does.
//
// Streams. parse-stream and read pull the input a line at a time
// (readLineBlocking, IO.h), so a document is never held as a whole beyond
// the line being parsed, and a value that ends a line is answered without
// waiting for more input (newline-delimited JSON over a pipe or a socket
// works). The rest of the line after the value is consumed with it (D57).
//
// GC (engineering principles P1/P2): every container is built in the
// caller's context, with no context of its own (CallerSlots.h) -- an array
// through a ListBuilder, an object with the map built so far in a slot and
// the pending entries in a batch under a SafepointBlock -- and the reading
// primitive holds a SafepointPermit, so the intermediate versions a large
// document leaves behind are reclaimed while it is still being read
// (tests/cli/json-under-heap-limit.sh). Values are returned unrooted and
// rooted by the caller before anything else allocates. A blocking read
// happens with every value parsed so far held in a slot or a live context.

#include "DataFormats.h"
#include "CallerSlots.h"

#include "ExecutionEngine.h"
#include "Exceptions.h"
#include "IO.h"
#include "LazySeq.h"
#include "ListBuilder.h"
#include "MapOps.h"
#include "Named.h"
#include "Primitives.h"
#include "SeqTypes.h"
#include "SetOps.h"
#include "StackGuard.h"
#include "Utf8String.h"
#include "VectorOps.h"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <string>
#include <system_error>

namespace protoClojure {

const char* ednPrimitiveName(proto::ProtoMethod fn);  // Edn.cpp

namespace {

using PO = proto::ProtoObject;

enum class Flavor { Cheshire, DataJson };

// The exception class a parse or write error raises: Jackson's
// JsonParseException / JsonGenerationException are IOExceptions;
// clojure.data.json throws java.lang.Exception.
const char* errorClass(Flavor f) { return f == Flavor::Cheshire ? "IOException" : "Exception"; }

// A malformed document. Thrown by the parser and turned into a classed
// exception by the primitive, once the parser's contexts are gone.
struct JsonSyntaxError {
    std::string message;
};

bool truthy(const PO* v) { return v && v != PROTO_NONE && v != PROTO_FALSE; }


// ============================================================== the source

// The text being parsed: a whole string, or a reader pulled a line at a
// time. Tracks the line and where it starts, for error positions.
class Source {
public:
    explicit Source(std::string text) : buf_(std::move(text)) {}
    explicit Source(int fd) : fd_(fd) {}

    // The byte at the current position, or -1 at the end of the input.
    int peek(proto::ProtoContext* ctx) {
        if (pos_ < buf_.size()) return static_cast<unsigned char>(buf_[pos_]);
        return refill(ctx) ? static_cast<unsigned char>(buf_[pos_]) : -1;
    }
    void advance() { ++pos_; }
    // After consuming a newline character.
    void newline() { ++line_; lineStart_ = pos_; }

    // The bytes still buffered from the current position; empty when a
    // refill is needed. For fast scans of string contents.
    const char* data() const { return buf_.data() + pos_; }
    std::size_t available() const { return buf_.size() - pos_; }
    void skip(std::size_t n) { pos_ += n; }

    // "line L, column C" of the current position (columns count characters).
    std::string where() const {
        long column = 1;
        for (std::size_t i = lineStart_; i < pos_ && i < buf_.size(); ++i)
            if ((static_cast<unsigned char>(buf_[i]) & 0xC0) != 0x80) ++column;
        return "line " + std::to_string(line_) + ", column " + std::to_string(column);
    }

    bool isStream() const { return fd_ >= 0; }

private:
    bool refill(proto::ProtoContext* ctx) {
        if (fd_ < 0 || eof_) return false;
        std::optional<std::string> line = readLineBlocking(ctx, fd_);
        if (!line) { eof_ = true; return false; }
        buf_ = std::move(*line);
        buf_ += '\n';  // the line end readLine strips; whitespace to JSON
        pos_ = 0;
        lineStart_ = 0;
        return true;
    }

    std::string buf_;
    std::size_t pos_ = 0;
    std::size_t lineStart_ = 0;
    long line_ = 1;
    int fd_ = -1;
    bool eof_ = false;
};

// ============================================================== the parser

struct ReadOptions {
    enum class Keys { Strings, Keywords, Fn };
    Keys keys = Keys::Strings;
    const PO* keyFn = nullptr;    // Keys::Fn; rooted by the caller
    const PO* valueFn = nullptr;  // data.json :value-fn; rooted by the caller
};

class Parser {
public:
    Parser(Source& src, const ReadOptions& opts, const ActiveCallContext& cc)
        : src_(src), opts_(opts), cc_(cc) {}

    // Skips whitespace; true when a value follows, false at the end.
    bool more(proto::ProtoContext* ctx) {
        skipWhitespace(ctx);
        return src_.peek(ctx) >= 0;
    }

    // The next value, unrooted. `more` must have answered true.
    const PO* value(proto::ProtoContext* ctx);

    [[noreturn]] void fail(const std::string& what) {
        throw JsonSyntaxError{"JSON error: " + what + " at " + src_.where()};
    }

    // A description of the character at the current position, for errors.
    std::string describeCurrent(proto::ProtoContext* ctx) {
        const int c = src_.peek(ctx);
        if (c < 0) return "end of input";
        if (c < 0x20) return "control character " + std::to_string(c);
        if (c >= 0x80) return "character beyond ASCII";
        return std::string("character '") + static_cast<char>(c) + "'";
    }

private:
    void skipWhitespace(proto::ProtoContext* ctx) {
        for (;;) {
            const int c = src_.peek(ctx);
            if (c == ' ' || c == '\t' || c == '\r') { src_.advance(); continue; }
            if (c == '\n') { src_.advance(); src_.newline(); continue; }
            return;
        }
    }
    void expect(proto::ProtoContext* ctx, char c, const char* what) {
        if (src_.peek(ctx) != static_cast<unsigned char>(c))
            fail(std::string("expected ") + what + ", found " + describeCurrent(ctx));
        src_.advance();
    }

    const PO* array(proto::ProtoContext* ctx);
    const PO* object(proto::ProtoContext* ctx);
    const PO* number(proto::ProtoContext* ctx);
    void string(proto::ProtoContext* ctx, std::string& out);
    unsigned hex4(proto::ProtoContext* ctx);
    void word(proto::ProtoContext* ctx, const char* w);
    const PO* makeKey(proto::ProtoContext* ctx, const std::string& text);

    Source& src_;
    const ReadOptions& opts_;
    const ActiveCallContext& cc_;
    std::string scratch_;
};

const PO* Parser::value(proto::ProtoContext* ctx) {
    checkNativeStack(StackUse::Source);
    skipWhitespace(ctx);
    const int c = src_.peek(ctx);
    switch (c) {
        case '{': src_.advance(); return object(ctx);
        case '[': src_.advance(); return array(ctx);
        case '"': {
            src_.advance();
            scratch_.clear();
            string(ctx, scratch_);
            return makeUtf8String(ctx, scratch_);
        }
        case 't': word(ctx, "true"); return PROTO_TRUE;
        case 'f': word(ctx, "false"); return PROTO_FALSE;
        case 'n': word(ctx, "null"); return PROTO_NONE;
        default: break;
    }
    if (c == '-' || (c >= '0' && c <= '9')) return number(ctx);
    if (c < 0) fail("unexpected end of input, expected a value");
    // Name the token as Jackson does ("Unrecognized token 'NaN'").
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        std::string tok;
        for (int d = src_.peek(ctx); (d >= 'a' && d <= 'z') || (d >= 'A' && d <= 'Z') ||
                                     (d >= '0' && d <= '9');
             d = src_.peek(ctx)) {
            tok += static_cast<char>(d);
            src_.advance();
        }
        fail("unrecognized token '" + tok + "', expected a value");
    }
    fail("unexpected " + describeCurrent(ctx) + ", expected a value");
}

void Parser::word(proto::ProtoContext* ctx, const char* w) {
    std::string seen;
    for (const char* p = w; *p; ++p) {
        const int c = src_.peek(ctx);
        if (c != static_cast<unsigned char>(*p)) {
            for (int d = c; (d >= 'a' && d <= 'z') || (d >= 'A' && d <= 'Z'); d = src_.peek(ctx)) {
                seen += static_cast<char>(d);
                src_.advance();
            }
            fail("unrecognized token '" + seen + "', expected a value");
        }
        seen += *p;
        src_.advance();
    }
    const int c = src_.peek(ctx);
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        while ((src_.peek(ctx) >= 'a' && src_.peek(ctx) <= 'z') ||
               (src_.peek(ctx) >= '0' && src_.peek(ctx) <= '9')) {
            seen += static_cast<char>(src_.peek(ctx));
            src_.advance();
        }
        fail("unrecognized token '" + seen + "', expected a value");
    }
}

const PO* Parser::number(proto::ProtoContext* ctx) {
    std::string& t = scratch_;
    t.clear();
    auto digit = [&](int c) { return c >= '0' && c <= '9'; };
    bool isFloat = false;
    if (src_.peek(ctx) == '-') { t += '-'; src_.advance(); }
    int c = src_.peek(ctx);
    if (c == '0') {
        t += '0';
        src_.advance();
        if (digit(src_.peek(ctx))) fail("invalid number, leading zeroes are not allowed");
    } else if (digit(c)) {
        while (digit(src_.peek(ctx))) { t += static_cast<char>(src_.peek(ctx)); src_.advance(); }
    } else {
        fail("invalid number, expected a digit after '-'");
    }
    if (src_.peek(ctx) == '.') {
        isFloat = true;
        t += '.';
        src_.advance();
        if (!digit(src_.peek(ctx))) fail("invalid number, expected a digit after the decimal point");
        while (digit(src_.peek(ctx))) { t += static_cast<char>(src_.peek(ctx)); src_.advance(); }
    }
    c = src_.peek(ctx);
    if (c == 'e' || c == 'E') {
        isFloat = true;
        t += 'e';
        src_.advance();
        c = src_.peek(ctx);
        if (c == '+' || c == '-') { t += static_cast<char>(c); src_.advance(); }
        if (!digit(src_.peek(ctx))) fail("invalid number, expected a digit in the exponent");
        while (digit(src_.peek(ctx))) { t += static_cast<char>(src_.peek(ctx)); src_.advance(); }
    }
    c = src_.peek(ctx);
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '_')
        fail("invalid number, unexpected " + describeCurrent(ctx));
    if (isFloat) {
        // strtod, as the lexer reads float literals (portable where
        // std::from_chars for doubles is not; the program never changes the
        // C locale). Overflow is an infinity and underflow a zero or a
        // subnormal, as Java's Double.parseDouble.
        return ctx->fromDouble(std::strtod(t.c_str(), nullptr));
    }
    long long v = 0;
    const auto r = std::from_chars(t.data(), t.data() + t.size(), v);
    if (r.ec == std::errc()) return ctx->fromLong(v);
    // Beyond 64 bits: an exact LargeInteger, as the reader builds one.
    return ctx->fromString(t.c_str(), 10);
}

unsigned Parser::hex4(proto::ProtoContext* ctx) {
    unsigned v = 0;
    for (int i = 0; i < 4; ++i) {
        const int h = src_.peek(ctx);
        unsigned d;
        if (h >= '0' && h <= '9') d = static_cast<unsigned>(h - '0');
        else if (h >= 'a' && h <= 'f') d = static_cast<unsigned>(h - 'a' + 10);
        else if (h >= 'A' && h <= 'F') d = static_cast<unsigned>(h - 'A' + 10);
        else fail("invalid \\u escape, expected four hexadecimal digits");
        v = v * 16 + d;
        src_.advance();
    }
    return v;
}

void appendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// The contents of a string, the opening quote already consumed, appended to
// `out` as UTF-8. An unescaped control character is accepted, as Cheshire's
// parser accepts it.
void Parser::string(proto::ProtoContext* ctx, std::string& out) {
    for (;;) {
        // Fast path: copy the run of plain bytes already buffered.
        const char* p = src_.data();
        const std::size_t n = src_.available();
        std::size_t i = 0;
        while (i < n && p[i] != '"' && p[i] != '\\' && p[i] != '\n') ++i;
        if (i > 0) { out.append(p, i); src_.skip(i); }
        const int c = src_.peek(ctx);
        if (c < 0) fail("unexpected end of input inside a string");
        src_.advance();
        if (c == '"') return;
        if (c == '\n') { out += '\n'; src_.newline(); continue; }
        // A backslash.
        const int e = src_.peek(ctx);
        if (e < 0) fail("unexpected end of input inside a string");
        src_.advance();
        switch (e) {
            case '"':  out += '"'; break;
            case '\\': out += '\\'; break;
            case '/':  out += '/'; break;
            case 'b':  out += '\b'; break;
            case 'f':  out += '\f'; break;
            case 'n':  out += '\n'; break;
            case 'r':  out += '\r'; break;
            case 't':  out += '\t'; break;
            case 'u': {
                unsigned cp = hex4(ctx);
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (src_.peek(ctx) != '\\') fail("unpaired surrogate in a \\u escape");
                    src_.advance();
                    if (src_.peek(ctx) != 'u') fail("unpaired surrogate in a \\u escape");
                    src_.advance();
                    const unsigned lo = hex4(ctx);
                    if (lo < 0xDC00 || lo > 0xDFFF) fail("unpaired surrogate in a \\u escape");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    fail("unpaired surrogate in a \\u escape");
                }
                appendUtf8(out, cp);
                break;
            }
            default:
                fail(e < 0x20 || e >= 0x80
                         ? std::string("invalid escape in a string")
                         : std::string("invalid escape '\\") + static_cast<char>(e) + "' in a string");
        }
    }
}

const PO* Parser::makeKey(proto::ProtoContext* ctx, const std::string& text) {
    switch (opts_.keys) {
        case ReadOptions::Keys::Keywords:
            return internNamed(ctx, cc_.named, (":" + text).c_str());
        case ReadOptions::Keys::Strings:
        case ReadOptions::Keys::Fn:
            break;
    }
    return makeUtf8String(ctx, text);
}

// The containers are built in the caller's context, with no context of
// their own (CallerSlots.h). An array's elements go to a ListBuilder, which
// hands each folded chunk's garbage to the collector when the reading
// primitive holds a SafepointPermit; an object's entries wait in a C++ batch,
// under a SafepointBlock, until they are folded into the map in its slot.
const PO* Parser::array(proto::ProtoContext* ctx) {
    CallerSlots scope(ctx, 1);
    {
        ListBuilder items(ctx);
        skipWhitespace(ctx);
        if (src_.peek(ctx) == ']') {
            src_.advance();
        } else {
            for (;;) {
                items.push(value(ctx));
                skipWhitespace(ctx);
                const int d = src_.peek(ctx);
                if (d == ',') { src_.advance(); continue; }
                if (d == ']') { src_.advance(); break; }
                if (d < 0) fail("unexpected end of input inside an array, expected ',' or ']'");
                fail("expected ',' or ']' in an array, found " + describeCurrent(ctx));
            }
        }
        scope.set(0, items.finish());
    }
    return scope.ret(newVector(ctx, scope.get(0)->asList(ctx)));
}

const PO* Parser::object(proto::ProtoContext* ctx) {
    // Slot 0: the map so far. The entries not yet in it wait in `kv`, a C++
    // array the collector does not trace, so `parked` blocks submissions
    // while any is there; each is a young cell of ctx or a callback's result
    // anchored in ctx, and nothing else submits ctx's chain meanwhile.
    constexpr unsigned kBatch = 16;
    CallerSlots scope(ctx, 1);
    SafepointBlock parked;
    const PO* kv[2 * kBatch];
    scope.set(0, mapAssocPairs(ctx, nullptr, nullptr, 0));
    unsigned pending = 0;
    auto fold = [&]() {
        if (pending == 0) return;
        scope.set(0, mapAssocPairs(ctx, scope.get(0), kv, 2 * pending));
        pending = 0;
        parked.release();
    };
    skipWhitespace(ctx);
    if (src_.peek(ctx) == '}') {
        src_.advance();
    } else {
        for (;;) {
            skipWhitespace(ctx);
            const int q = src_.peek(ctx);
            if (q != '"') {
                if (q < 0) fail("unexpected end of input inside an object, expected a key");
                fail("expected a string key in an object, found " + describeCurrent(ctx));
            }
            src_.advance();
            std::string keyText;
            string(ctx, keyText);
            parked.hold();
            const unsigned ks = 2 * pending, vs = ks + 1;
            kv[ks] = makeKey(ctx, keyText);
            kv[vs] = PROTO_NONE;
            if (opts_.keys == ReadOptions::Keys::Fn) {
                const PO* a[1] = {kv[ks]};
                kv[ks] = cc_.engine->invoke(ctx, opts_.keyFn, a, 1);
            }
            skipWhitespace(ctx);
            expect(ctx, ':', "':' after an object key");
            kv[vs] = value(ctx);
            bool keep = true;
            if (opts_.valueFn) {
                // data.json: (value-fn key value); answering value-fn itself
                // drops the entry.
                const PO* a[2] = {kv[ks], kv[vs]};
                const PO* r = cc_.engine->invoke(ctx, opts_.valueFn, a, 2);
                if (r == opts_.valueFn) keep = false;
                else kv[vs] = r ? r : PROTO_NONE;
            }
            if (keep) {
                if (++pending == kBatch) fold();
            } else if (pending == 0) {
                parked.release();
            }
            skipWhitespace(ctx);
            const int d = src_.peek(ctx);
            if (d == ',') { src_.advance(); continue; }
            if (d == '}') { src_.advance(); break; }
            if (d < 0) fail("unexpected end of input inside an object, expected ',' or '}'");
            fail("expected ',' or '}' in an object, found " + describeCurrent(ctx));
        }
    }
    fold();
    return scope.ret(scope.get(0));
}

// ============================================================== the writer

struct WriteOptions {
    Flavor flavor = Flavor::Cheshire;
    bool pretty = false;              // cheshire :pretty
    bool escapeNonAscii = false;      // cheshire :escape-non-ascii, data.json :escape-unicode
    bool escapeSlash = false;         // data.json :escape-slash
    bool escapeJsSeparators = false;  // data.json :escape-js-separators
    const PO* keyFn = nullptr;        // rooted by the caller
};

class Writer {
public:
    Writer(std::string& out, const WriteOptions& opts, const ActiveCallContext& cc)
        : out_(out), opts_(opts), cc_(cc) {}

    void value(proto::ProtoContext* ctx, const PO* v);

private:
    void string(const std::string& s);
    void hexEscape(unsigned u) {
        static const char* const kUpper = "0123456789ABCDEF";
        static const char* const kLower = "0123456789abcdef";
        const char* digits = opts_.flavor == Flavor::Cheshire ? kUpper : kLower;
        out_ += "\\u";
        for (int shift = 12; shift >= 0; shift -= 4) out_ += digits[(u >> shift) & 0xF];
    }
    void newlineIndent() {
        out_ += '\n';
        out_.append(static_cast<std::size_t>(2 * depth_), ' ');
    }
    std::string keyText(proto::ProtoContext* ctx, const PO* k);
    std::string namedText(proto::ProtoContext* ctx, const PO* v) const;
    [[noreturn]] void cannotWrite(proto::ProtoContext* ctx, const PO* v);
    void sequence(proto::ProtoContext* ctx, const PO* v);
    void map(proto::ProtoContext* ctx, const PO* m);

    std::string& out_;
    const WriteOptions& opts_;
    const ActiveCallContext& cc_;
    int depth_ = 0;  // enclosing objects, for :pretty
};

// A keyword's or a symbol's text: cheshire keeps the namespace (`:a/b` is
// "a/b"), clojure.data.json writes the name ("b").
std::string Writer::namedText(proto::ProtoContext* ctx, const PO* v) const {
    std::string s = namedSpelling(ctx, cc_.named, v)->toStdString(ctx);
    if (!s.empty() && s[0] == ':') s.erase(0, 1);
    if (opts_.flavor == Flavor::DataJson) {
        const std::size_t slash = s.find('/');
        if (slash != std::string::npos && slash > 0 && slash + 1 < s.size()) s.erase(0, slash + 1);
    }
    return s;
}

void Writer::cannotWrite(proto::ProtoContext* ctx, const PO* v) {
    std::string type = valueTypeName(ctx, v);
    if (type.compare(0, 2, "a ") == 0) type.erase(0, 2);
    else if (type.compare(0, 3, "an ") == 0) type.erase(0, 3);
    if (opts_.flavor == Flavor::Cheshire)
        throwClassed(ctx, "IOException", "Cannot JSON encode object of class: " + type);
    throwClassed(ctx, "Exception", "Don't know how to write JSON of " + type);
}

void Writer::string(const std::string& s) {
    out_ += '"';
    const std::size_t n = s.size();
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            switch (c) {
                case '"':  out_ += "\\\""; continue;
                case '\\': out_ += "\\\\"; continue;
                case '\b': out_ += "\\b"; continue;
                case '\f': out_ += "\\f"; continue;
                case '\n': out_ += "\\n"; continue;
                case '\r': out_ += "\\r"; continue;
                case '\t': out_ += "\\t"; continue;
                case '/':  out_ += opts_.escapeSlash ? "\\/" : "/"; continue;
                default: break;
            }
            if (c < 0x20) hexEscape(c);
            else out_ += static_cast<char>(c);
            continue;
        }
        if (!opts_.escapeNonAscii && !opts_.escapeJsSeparators) {
            out_ += static_cast<char>(c);
            continue;
        }
        // Decode one UTF-8 sequence; a malformed one is U+FFFD.
        unsigned cp = 0xFFFD;
        std::size_t len = 1;
        if (c >= 0xC2 && c <= 0xDF) len = 2, cp = c & 0x1F;
        else if (c >= 0xE0 && c <= 0xEF) len = 3, cp = c & 0x0F;
        else if (c >= 0xF0 && c <= 0xF4) len = 4, cp = c & 0x07;
        bool ok = len > 1 && i + len <= n;
        for (std::size_t k = 1; ok && k < len; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) ok = false;
            else cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) { cp = 0xFFFD; len = 1; }
        const bool separator = cp == 0x2028 || cp == 0x2029;
        if (opts_.escapeNonAscii || (separator && opts_.escapeJsSeparators)) {
            if (cp >= 0x10000) {
                const unsigned u = cp - 0x10000;
                hexEscape(0xD800 + (u >> 10));
                hexEscape(0xDC00 + (u & 0x3FF));
            } else {
                hexEscape(cp);
            }
        } else if (ok) {
            out_.append(s, i, len);
        } else {
            out_ += "\xEF\xBF\xBD";
        }
        i += len - 1;
    }
    out_ += '"';
}

// The text an object key is written as.
std::string Writer::keyText(proto::ProtoContext* ctx, const PO* k) {
    if (k == nullptr || k == PROTO_NONE) {
        if (opts_.flavor == Flavor::DataJson && !opts_.keyFn)
            throwClassed(ctx, "Exception", "JSON object properties may not be nil");
        if (!opts_.keyFn) return "";
    }
    const bool named = k && k != PROTO_NONE && isNamed(ctx, cc_.named, k);
    const bool keyword = named && namedSpelling(ctx, cc_.named, k)->toStdString(ctx)[0] == ':';
    // cheshire applies :key-fn to keyword keys only; data.json to every key.
    if (opts_.keyFn && (opts_.flavor == Flavor::DataJson || keyword)) {
        CallerSlots scope(ctx, 1);
        const PO* a[1] = {k ? k : PROTO_NONE};
        scope.set(0, cc_.engine->invoke(ctx, opts_.keyFn, a, 1));
        const PO* r = scope.get(0);
        if (r && r != PROTO_NONE && proto::ProtoObject::isStringTagFast(r))
            return reinterpret_cast<const proto::ProtoString*>(r)->toStdString(ctx);
        std::string s;
        printValueTo(ctx, s, r ? r : PROTO_NONE, false);
        return s;
    }
    if (k == nullptr || k == PROTO_NONE) return "";
    if (proto::ProtoObject::isStringTagFast(k))
        return reinterpret_cast<const proto::ProtoString*>(k)->toStdString(ctx);
    if (named) return namedText(ctx, k);
    std::string s;
    printValueTo(ctx, s, k, false);
    return s;
}

void Writer::map(proto::ProtoContext* ctx, const PO* m) {
    if (mapIsEmpty(ctx, m)) {
        out_ += opts_.pretty ? "{ }" : "{}";
        return;
    }
    out_ += '{';
    ++depth_;
    struct Acc { Writer* w; bool first; } acc{this, true};
    mapForEach(ctx, m, &acc, [](proto::ProtoContext* c, void* self, const PO* k, const PO* v) {
        auto* a = static_cast<Acc*>(self);
        Writer& w = *a->w;
        if (!a->first) w.out_ += ',';
        a->first = false;
        if (w.opts_.pretty) w.newlineIndent();
        w.string(w.keyText(c, k));
        w.out_ += w.opts_.pretty ? " : " : ":";
        w.value(c, v);
    });
    --depth_;
    if (opts_.pretty) newlineIndent();
    out_ += '}';
}

void Writer::sequence(proto::ProtoContext* ctx, const PO* v) {
    out_ += '[';
    bool first = true;
    auto element = [&](proto::ProtoContext* c, const PO* e) {
        if (!first) out_ += opts_.pretty ? ", " : ",";
        else if (opts_.pretty) out_ += ' ';
        first = false;
        value(c, e);
    };
    if (isSet(ctx, v)) {
        struct Acc { decltype(element)* f; } acc{&element};
        setForEach(ctx, v, &acc, [](proto::ProtoContext* c, void* self, const PO* e) {
            (*static_cast<Acc*>(self)->f)(c, e);
        });
    } else {
        CallerSlots scope(ctx, 1);
        for (SeqWalk w(scope, 0, ctx, v); !w.done(); w.advance(ctx))
            element(ctx, w.first(ctx));
    }
    if (opts_.pretty) out_ += ' ';  // "[ ]", "[ 1, 2 ]", as Jackson
    out_ += ']';
}

void Writer::value(proto::ProtoContext* ctx, const PO* v) {
    checkNativeStack();
    if (v == nullptr || v == PROTO_NONE) { out_ += "null"; return; }
    if (v == PROTO_TRUE) { out_ += "true"; return; }
    if (v == PROTO_FALSE) { out_ += "false"; return; }
    if (v->isInteger(ctx)) {
        if (proto::isSmallInt(v)) out_ += std::to_string(proto::asSmallInt(v));
        else out_ += v->asIntegerString(ctx, 10)->toStdString(ctx);
        return;
    }
    if (v->isFloat(ctx)) {
        const double d = v->asDouble(ctx);
        if (std::isfinite(d)) { out_ += formatDouble(d); return; }
        if (opts_.flavor == Flavor::DataJson)
            throwClassed(ctx, "Exception", std::isnan(d) ? "JSON error: cannot write Double NaN"
                                                         : "JSON error: cannot write infinite Double");
        // Jackson quotes the non-numeric numbers.
        out_ += std::isnan(d) ? "\"NaN\"" : (d > 0 ? "\"Infinity\"" : "\"-Infinity\"");
        return;
    }
    if (proto::ProtoObject::isStringTagFast(v)) {
        string(reinterpret_cast<const proto::ProtoString*>(v)->toStdString(ctx));
        return;
    }
    if (isMap(v)) { map(ctx, v); return; }
    // A list is a seq object (SeqTypes.h).
    if (isVector(v) || isSeqObject(ctx, v) || isSet(ctx, v)) {
        sequence(ctx, v);
        return;
    }
    if (isNamed(ctx, cc_.named, v)) { string(namedText(ctx, v)); return; }
    cannotWrite(ctx, v);
}

// ============================================================== arguments

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
std::string stringArg(proto::ProtoContext* ctx, const PO* v, const char* who) {
    if (!v || v == PROTO_NONE || !proto::ProtoObject::isStringTagFast(v))
        throwClassed(ctx, "ClassCastException",
                     std::string(who) + " expects a string, got " + valueTypeName(ctx, v));
    return reinterpret_cast<const proto::ProtoString*>(v)->toStdString(ctx);
}
const PO* keywordValue(proto::ProtoContext* ctx, const ActiveCallContext& cc, const char* spelling) {
    return internNamed(ctx, cc.named, spelling);
}

// The option `name` (a keyword spelling) in `opts`: a map, or nil.
const PO* mapOption(proto::ProtoContext* ctx, const ActiveCallContext& cc, const PO* opts,
                    const char* name, bool* present = nullptr) {
    bool found = false;
    const PO* v = PROTO_NONE;
    if (opts != PROTO_NONE && isMap(opts)) v = mapGet(ctx, opts, keywordValue(ctx, cc, name), &found);
    if (present) *present = found;
    return v ? v : PROTO_NONE;
}

// clojure.data.json's options: keyword / value pairs after the positional
// arguments, gathered into a map.
const PO* pairOptions(proto::ProtoContext* ctx, const proto::ProtoArguments* args,
                      proto::proto_ulong from, const char* who) {
    const proto::proto_ulong n = argc(ctx, args);
    if (from >= n) return PROTO_NONE;
    if ((n - from) % 2 != 0)
        throwClassed(ctx, "IllegalArgumentException",
                     std::string(who) + ": options must be keyword / value pairs");
    std::vector<const PO*> kv;
    for (proto::proto_ulong i = from; i < n; ++i) kv.push_back(argAt(ctx, args, i));
    return mapAssocPairs(ctx, nullptr, kv.data(), kv.size());
}

// The key mode a key function names: `keyword` itself is done in C++.
void setKeyFn(proto::ProtoContext* ctx, const ActiveCallContext& cc, const PO* fn, ReadOptions& o) {
    if (!truthy(fn)) return;
    if (fn == PROTO_TRUE ||
        fn == cc.globals->getAttribute(ctx, proto::ProtoString::createSymbol(ctx, "clojure.core/keyword"))) {
        o.keys = ReadOptions::Keys::Keywords;
        return;
    }
    o.keys = ReadOptions::Keys::Fn;
    o.keyFn = fn;
}

// ============================================================== reading

enum class Trailing { Ignore, Reject };

// The first value of `src`. A source with no value answers `eofValue` when
// `eofOk`, else raises; with Trailing::Reject anything but whitespace after
// the value is an error (clojure.data.json).
const PO* readJson(proto::ProtoContext* ctx, const ActiveCallContext& cc, Source& src,
                   const ReadOptions& opts, Flavor flavor, Trailing trailing, bool eofOk,
                   const PO* eofValue) {
    std::string error;
    {
        // The reading primitives hold their options in their arguments or in
        // a slot, and the parser keeps its partial containers in slots or
        // under a SafepointBlock: a large document hands its garbage to the
        // collector while it is read (CallerSlots.h).
        SafepointPermit permit(ctx);
        CallerSlots scope(ctx, 1);
        try {
            Parser p(src, opts, cc);
            if (!p.more(ctx)) {
                if (eofOk) return eofValue;
                p.fail("unexpected end of input, expected a value");
            }
            scope.set(0, p.value(ctx));
            if (trailing == Trailing::Reject && p.more(ctx))
                p.fail("found extra data after the value: " + p.describeCurrent(ctx));
            return scope.ret(scope.get(0));
        } catch (const JsonSyntaxError& e) {
            error = e.message;
        }
    }
    throwClassed(ctx, errorClass(flavor), error);
}

#define JSONPRIM(name) \
    const PO* name(proto::ProtoContext* ctx, const PO*, const proto::ParentLink*, \
                   const proto::ProtoArguments* args, const proto::ProtoKeywordArguments*)

// (parse-string s), (parse-string s key-fn): key-fn true gives keyword keys,
// a fn is applied to each key; nil for nil or a blank string; whatever
// follows the first value is ignored, as Jackson ignores it.
JSONPRIM(prim_parse_string) {
    const proto::proto_ulong n = argc(ctx, args);
    if (n < 1 || n > 3) arity(ctx, n, "parse-string");
    const ActiveCallContext& cc = callContext(ctx, "parse-string");
    const PO* s = argAt(ctx, args, 0);
    if (s == PROTO_NONE) return PROTO_NONE;
    if (n == 3 && truthy(argAt(ctx, args, 2)))
        throwClassed(ctx, "UnsupportedOperationException",
                     "parse-string: array-coerce-fn is not supported (D57)");
    ReadOptions o;
    if (n >= 2) setKeyFn(ctx, cc, argAt(ctx, args, 1), o);
    Source src(stringArg(ctx, s, "parse-string"));
    return readJson(ctx, cc, src, o, Flavor::Cheshire, Trailing::Ignore, true, PROTO_NONE);
}

// (parse-stream rdr), (parse-stream rdr key-fn): the next value of a reader;
// nil at its end.
JSONPRIM(prim_parse_stream) {
    const proto::proto_ulong n = argc(ctx, args);
    if (n < 1 || n > 3) arity(ctx, n, "parse-stream");
    const ActiveCallContext& cc = callContext(ctx, "parse-stream");
    if (n == 3 && truthy(argAt(ctx, args, 2)))
        throwClassed(ctx, "UnsupportedOperationException",
                     "parse-stream: array-coerce-fn is not supported (D57)");
    ReadOptions o;
    if (n >= 2) setKeyFn(ctx, cc, argAt(ctx, args, 1), o);
    Source src(readableDescriptor(ctx, argAt(ctx, args, 0), "parse-stream"));
    return readJson(ctx, cc, src, o, Flavor::Cheshire, Trailing::Ignore, true, PROTO_NONE);
}

// The read options of clojure.data.json: :key-fn, :value-fn, :eof-error?,
// :eof-value, :bigdec.
void dataJsonReadOptions(proto::ProtoContext* ctx, const ActiveCallContext& cc, const PO* opts,
                         ReadOptions& o, bool& eofOk, const PO*& eofValue, const char* who) {
    setKeyFn(ctx, cc, mapOption(ctx, cc, opts, ":key-fn"), o);
    const PO* vf = mapOption(ctx, cc, opts, ":value-fn");
    if (truthy(vf)) o.valueFn = vf;
    bool present = false;
    const PO* eofError = mapOption(ctx, cc, opts, ":eof-error?", &present);
    eofOk = present && !truthy(eofError);
    eofValue = mapOption(ctx, cc, opts, ":eof-value");
    if (truthy(mapOption(ctx, cc, opts, ":bigdec")))
        throwClassed(ctx, "UnsupportedOperationException",
                     std::string(who) + ": :bigdec needs BigDecimal, which protoClojure does not have (D57)");
}

// (read-str s & opts): the value of s; extra data after it is an error.
JSONPRIM(prim_read_str) {
    const proto::proto_ulong n = argc(ctx, args);
    if (n < 1) arity(ctx, n, "read-str");
    const ActiveCallContext& cc = callContext(ctx, "read-str");
    CallerSlots scope(ctx, 1);
    scope.set(0, pairOptions(ctx, args, 1, "read-str"));
    ReadOptions o;
    bool eofOk = false;
    const PO* eofValue = PROTO_NONE;
    dataJsonReadOptions(ctx, cc, scope.get(0), o, eofOk, eofValue, "read-str");
    Source src(stringArg(ctx, argAt(ctx, args, 0), "read-str"));
    const PO* v = readJson(ctx, cc, src, o, Flavor::DataJson, Trailing::Reject, eofOk, eofValue);
    return v;
}

// (read rdr & opts): the next value of a reader.
JSONPRIM(prim_read) {
    const proto::proto_ulong n = argc(ctx, args);
    if (n < 1) arity(ctx, n, "read");
    const ActiveCallContext& cc = callContext(ctx, "read");
    CallerSlots scope(ctx, 1);
    scope.set(0, pairOptions(ctx, args, 1, "read"));
    ReadOptions o;
    bool eofOk = false;
    const PO* eofValue = PROTO_NONE;
    dataJsonReadOptions(ctx, cc, scope.get(0), o, eofOk, eofValue, "read");
    Source src(readableDescriptor(ctx, argAt(ctx, args, 0), "clojure.data.json/read"));
    const PO* v = readJson(ctx, cc, src, o, Flavor::DataJson, Trailing::Ignore, eofOk, eofValue);
    return v;
}

// ============================================================== writing

std::string writeJson(proto::ProtoContext* ctx, const ActiveCallContext& cc, const PO* v,
                      const WriteOptions& o) {
    std::string out;
    Writer w(out, o, cc);
    w.value(ctx, v);
    return out;
}

WriteOptions cheshireWriteOptions(proto::ProtoContext* ctx, const ActiveCallContext& cc,
                                  const PO* opts, const char* who) {
    if (opts != PROTO_NONE && !isMap(opts))
        throwClassed(ctx, "ClassCastException",
                     std::string(who) + ": options must be a map, got " + valueTypeName(ctx, opts));
    WriteOptions o;
    o.flavor = Flavor::Cheshire;
    o.pretty = truthy(mapOption(ctx, cc, opts, ":pretty"));
    o.escapeNonAscii = truthy(mapOption(ctx, cc, opts, ":escape-non-ascii"));
    const PO* kf = mapOption(ctx, cc, opts, ":key-fn");
    if (truthy(kf)) o.keyFn = kf;
    return o;
}

WriteOptions dataJsonWriteOptions(proto::ProtoContext* ctx, const ActiveCallContext& cc,
                                  const PO* opts) {
    WriteOptions o;
    o.flavor = Flavor::DataJson;
    bool present = false;
    const PO* v = mapOption(ctx, cc, opts, ":escape-unicode", &present);
    o.escapeNonAscii = !present || truthy(v);
    v = mapOption(ctx, cc, opts, ":escape-slash", &present);
    o.escapeSlash = !present || truthy(v);
    v = mapOption(ctx, cc, opts, ":escape-js-separators", &present);
    o.escapeJsSeparators = !present || truthy(v);
    const PO* kf = mapOption(ctx, cc, opts, ":key-fn");
    if (truthy(kf)) o.keyFn = kf;
    return o;
}

// (generate-string x), (generate-string x opts): opts :pretty,
// :escape-non-ascii, :key-fn (applied to keyword keys).
JSONPRIM(prim_generate_string) {
    const proto::proto_ulong n = argc(ctx, args);
    if (n < 1 || n > 2) arity(ctx, n, "generate-string");
    const ActiveCallContext& cc = callContext(ctx, "generate-string");
    const WriteOptions o =
        cheshireWriteOptions(ctx, cc, n == 2 ? argAt(ctx, args, 1) : PROTO_NONE, "generate-string");
    return makeUtf8String(ctx, writeJson(ctx, cc, argAt(ctx, args, 0), o));
}

// (generate-stream x writer), (generate-stream x writer opts): writes x to
// the writer and answers the writer, as Cheshire does.
JSONPRIM(prim_generate_stream) {
    const proto::proto_ulong n = argc(ctx, args);
    if (n < 2 || n > 3) arity(ctx, n, "generate-stream");
    const ActiveCallContext& cc = callContext(ctx, "generate-stream");
    const int fd = writableDescriptor(ctx, argAt(ctx, args, 1), "generate-stream");
    const WriteOptions o =
        cheshireWriteOptions(ctx, cc, n == 3 ? argAt(ctx, args, 2) : PROTO_NONE, "generate-stream");
    writeBlocking(ctx, fd, writeJson(ctx, cc, argAt(ctx, args, 0), o));
    return argAt(ctx, args, 1);
}

// (write-str x & opts): opts :escape-unicode, :escape-slash and
// :escape-js-separators (all true by default, as in clojure.data.json),
// :key-fn (applied to every key).
JSONPRIM(prim_write_str) {
    const proto::proto_ulong n = argc(ctx, args);
    if (n < 1) arity(ctx, n, "write-str");
    const ActiveCallContext& cc = callContext(ctx, "write-str");
    CallerSlots scope(ctx, 1);
    scope.set(0, pairOptions(ctx, args, 1, "write-str"));
    const WriteOptions o = dataJsonWriteOptions(ctx, cc, scope.get(0));
    const PO* s = makeUtf8String(ctx, writeJson(ctx, cc, argAt(ctx, args, 0), o));
    return s;
}

// (write x writer & opts): writes x to the writer; nil.
JSONPRIM(prim_write) {
    const proto::proto_ulong n = argc(ctx, args);
    if (n < 2) arity(ctx, n, "write");
    const ActiveCallContext& cc = callContext(ctx, "write");
    const int fd = writableDescriptor(ctx, argAt(ctx, args, 1), "clojure.data.json/write");
    CallerSlots scope(ctx, 1);
    scope.set(0, pairOptions(ctx, args, 2, "write"));
    const WriteOptions o = dataJsonWriteOptions(ctx, cc, scope.get(0));
    writeBlocking(ctx, fd, writeJson(ctx, cc, argAt(ctx, args, 0), o));
    return PROTO_NONE;
}

#undef JSONPRIM

struct JsonPrim { const char* name; proto::ProtoMethod fn; };
constexpr JsonPrim kCheshire[] = {
    {"parse-string",    &prim_parse_string},
    {"decode",          &prim_parse_string},
    {"parse-stream",    &prim_parse_stream},
    {"generate-string", &prim_generate_string},
    {"encode",          &prim_generate_string},
    {"generate-stream", &prim_generate_stream},
};
constexpr JsonPrim kDataJson[] = {
    {"read-str",  &prim_read_str},
    {"read",      &prim_read},
    {"write-str", &prim_write_str},
    {"write",     &prim_write},
};

void installNamespaceObject(proto::ProtoContext* ctx, proto::ProtoObject* globals,
                            const char* name, const JsonPrim* prims, std::size_t count) {
    CallerSlots scope(ctx, 1);
    scope.set(0, ctx->space->objectPrototype->newChild(ctx, false));
    for (std::size_t i = 0; i < count; ++i)
        scope.set(0, scope.get(0)->setAttribute(
            ctx, proto::ProtoString::createSymbol(ctx, prims[i].name),
            ctx->fromMethod(nullptr, prims[i].fn)));
    globals->setAttribute(ctx, proto::ProtoString::createSymbol(ctx, name),
                          scope.get(0));
}

} // namespace

void installJson(proto::ProtoContext* ctx, proto::ProtoObject* globals) {
    installNamespaceObject(ctx, globals, "cheshire.core", kCheshire,
                           sizeof kCheshire / sizeof *kCheshire);
    installNamespaceObject(ctx, globals, "clojure.data.json", kDataJson,
                           sizeof kDataJson / sizeof *kDataJson);
}

const char* dataFormatPrimitiveName(proto::ProtoMethod fn) {
    for (const JsonPrim& p : kCheshire)
        if (p.fn == fn) return p.name;
    for (const JsonPrim& p : kDataJson)
        if (p.fn == fn) return p.name;
    return ednPrimitiveName(fn);
}

} // namespace protoClojure
