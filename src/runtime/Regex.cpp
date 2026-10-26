#include "CallerSlots.h"
#include "Regex.h"
#include "StackGuard.h"

#include "protoCore.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>

namespace protoClojure {

namespace {

// Set once per ProtoSpace by initRegex; rooted by the caller.
const proto::ProtoObject* g_patternPrototype = nullptr;
const proto::ProtoObject* g_matcherPrototype = nullptr;

std::mutex g_cacheMutex;
// Never erased: a CompiledPattern's address is stored in pattern values.
std::unordered_map<std::string, std::unique_ptr<CompiledPattern>> g_cache;

const proto::ProtoString* sourceKey(proto::ProtoContext* ctx) {
    return proto::ProtoString::createSymbol(ctx, "__pattern__");
}
const proto::ProtoString* regexKey(proto::ProtoContext* ctx) {
    return proto::ProtoString::createSymbol(ctx, "__regex__");
}

bool isObjectCell(const proto::ProtoObject* v) {
    return v && (reinterpret_cast<proto::proto_ulong>(v) & 0x3F) == 0;
}

// ------------------------------------------------------------ translation

[[noreturn]] void syntaxError(const std::string& what, const std::string& src) {
    throw PatternSyntaxError(what + " in pattern: " + src);
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string hexEscape(unsigned long cp) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "\\x{%lx}", cp);
    return buf;
}

// The four hex digits of a `\u` escape at src[i] (the backslash), or -1.
long unicodeEscapeAt(const std::string& src, std::size_t i) {
    if (src.compare(i, 2, "\\u") != 0 || i + 6 > src.size()) return -1;
    long v = 0;
    for (std::size_t k = i + 2; k < i + 6; ++k) {
        const int h = hexValue(src[k]);
        if (h < 0) return -1;
        v = v * 16 + h;
    }
    return v;
}

// Java's POSIX character classes (ASCII unless UNICODE_CHARACTER_CLASS) and
// the PCRE2 POSIX class of the same set.
const char* posixClassOf(const std::string& name) {
    static const std::pair<const char*, const char*> kClasses[] = {
        {"Lower", "lower"}, {"Upper", "upper"}, {"ASCII", "ascii"}, {"Alpha", "alpha"},
        {"Digit", "digit"}, {"Alnum", "alnum"}, {"Punct", "punct"}, {"Graph", "graph"},
        {"Print", "print"}, {"Blank", "blank"}, {"Cntrl", "cntrl"}, {"XDigit", "xdigit"},
        {"Space", "space"},
    };
    for (const auto& [java, pcre] : kClasses)
        if (name == java) return pcre;
    return nullptr;
}

// The length of the UTF-8 sequence that starts with byte `lead` (1 for a
// byte that cannot start one).
std::size_t utf8Length(unsigned char lead) {
    if (lead < 0xC0) return 1;
    if (lead < 0xE0) return 2;
    if (lead < 0xF0) return 3;
    if (lead < 0xF8) return 4;
    return 1;
}

} // namespace

std::string translatePattern(const std::string& src, unsigned& extraOptions, bool& unixLines) {
    extraOptions = 0;
    unixLines = false;
    std::string out;
    out.reserve(src.size() + 8);
    // Character classes: `depth` counts the open brackets of the Java class
    // (a nested one is flattened into its outer class), `negated` whether
    // the outermost one is, `justOpened` whether the last thing written was
    // the class's `[` or `[^` (where PCRE2 would read `[:`, `[.` or `[=` as
    // POSIX syntax).
    int depth = 0;
    bool negated = false, justOpened = false;
    bool leading = true;  // nothing but inline flag groups written so far
    std::size_t i = 0;
    auto emitInClass = [&](const std::string& piece) {
        if (justOpened && (piece[0] == ':' || piece[0] == '.' || piece[0] == '=')) out += '\\';
        out += piece;
        justOpened = false;
    };
    while (i < src.size()) {
        const char c = src[i];
        if (c == '\\' && i + 1 < src.size()) {
            const char n = src[i + 1];
            if (n == 'Q') {
                // \Q...\E: copied as it is, PCRE2 reads it as Java does.
                const std::size_t e = src.find("\\E", i + 2);
                const std::size_t stop = e == std::string::npos ? src.size() : e + 2;
                if (depth > 0) emitInClass(src.substr(i, stop - i));
                else out.append(src, i, stop - i);
                i = stop;
                leading = false;
                continue;
            }
            std::string piece;
            if (n == 'u') {
                long v = unicodeEscapeAt(src, i);
                if (v < 0) syntaxError("Illegal Unicode escape sequence", src);
                std::size_t len = 6;
                if (v >= 0xD800 && v <= 0xDBFF) {
                    const long lo = unicodeEscapeAt(src, i + 6);
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
                        len = 12;
                    }
                }
                if (v >= 0xD800 && v <= 0xDFFF) syntaxError("Unpaired surrogate in a Unicode escape", src);
                piece = hexEscape(static_cast<unsigned long>(v));
                i += len;
            } else if (n == '0') {
                // Java: \0n, \0nn, \0mnn (m <= 3), always octal.
                std::size_t k = i + 2;
                unsigned long v = 0;
                const std::size_t maxDigits =
                    (k < src.size() && src[k] >= '0' && src[k] <= '3') ? 3 : 2;
                std::size_t digits = 0;
                while (digits < maxDigits && k < src.size() && src[k] >= '0' && src[k] <= '7') {
                    v = v * 8 + static_cast<unsigned long>(src[k] - '0');
                    ++k;
                    ++digits;
                }
                if (digits == 0) syntaxError("Illegal octal escape sequence", src);
                piece = hexEscape(v);
                i = k;
            } else if ((n == 'p' || n == 'P') && i + 2 < src.size()) {
                std::string name;
                std::size_t stop;
                if (src[i + 2] == '{') {
                    const std::size_t close = src.find('}', i + 3);
                    if (close == std::string::npos) syntaxError("Unclosed character family", src);
                    name = src.substr(i + 3, close - i - 3);
                    stop = close + 1;
                } else {
                    name = src.substr(i + 2, 1);
                    stop = i + 3;
                }
                if (const char* posix = posixClassOf(name)) {
                    const std::string cls = std::string("[:") + (n == 'P' ? "^" : "") + posix + ":]";
                    piece = depth > 0 ? cls : "[" + cls + "]";
                } else {
                    if (name.size() > 2 && name.compare(0, 2, "Is") == 0) name.erase(0, 2);
                    piece = std::string("\\") + n + "{" + name + "}";
                }
                i = stop;
            } else if (n == 'N') {
                syntaxError("Named characters (\\N{...}) are not supported", src);
            } else {
                // Any other escape: the backslash and the whole character.
                const std::size_t len = 1 + utf8Length(static_cast<unsigned char>(n));
                piece = src.substr(i, len);
                i += len;
            }
            if (depth > 0) {
                emitInClass(piece);
            } else {
                out += piece;
            }
            leading = false;
            continue;
        }
        if (depth > 0) {
            if (c == '[') {
                // A nested class: Java's union, flattened into the outer one.
                if (negated || (i + 1 < src.size() && src[i + 1] == '^'))
                    syntaxError("A negated nested character class is not supported", src);
                ++depth;
                ++i;
                continue;
            }
            if (c == '&' && src.compare(i, 2, "&&") == 0)
                syntaxError("Character class intersection (&&) is not supported", src);
            if (c == ']') {
                if (--depth == 0) out += ']';
                justOpened = false;
                ++i;
                continue;
            }
            const std::size_t len = utf8Length(static_cast<unsigned char>(c));
            emitInClass(src.substr(i, len));
            i += len;
            continue;
        }
        if (c == '[') {
            depth = 1;
            out += '[';
            ++i;
            negated = i < src.size() && src[i] == '^';
            if (negated) { out += '^'; ++i; }
            // A `]` right after `[` or `[^` is a literal member, as in Java.
            if (i < src.size() && src[i] == ']') { out += "\\]"; ++i; justOpened = false; }
            else justOpened = true;
            leading = false;
            continue;
        }
        if (c == '(' && src.compare(i, 2, "(?") == 0 && i + 2 < src.size() &&
            (std::isalpha(static_cast<unsigned char>(src[i + 2])) || src[i + 2] == '-')) {
            // An inline flag group, (?on-off) or (?on-off:X).
            std::size_t j = i + 2;
            std::string on, off;
            bool minus = false;
            while (j < src.size() && (std::isalpha(static_cast<unsigned char>(src[j])) || src[j] == '-')) {
                if (src[j] == '-') {
                    if (minus) syntaxError("Unknown inline modifier", src);
                    minus = true;
                } else {
                    const char f = src[j];
                    if (std::string("idmsuxU").find(f) == std::string::npos)
                        syntaxError(std::string("Unknown inline modifier (?") + f + ")", src);
                    if (f == 'd' || f == 'U') {
                        if (minus || !leading)
                            syntaxError(std::string("The inline flag (?") + f +
                                            ") is supported only at the start of the pattern", src);
                        if (f == 'd') unixLines = true;
                        else extraOptions |= 0x00020000u;  // PCRE2_UCP
                    } else if (f != 'u') {
                        const std::string add = f == 'x' && !minus ? "xx" : std::string(1, f);
                        (minus ? off : on) += add;
                    }
                }
                ++j;
            }
            if (j >= src.size() || (src[j] != ')' && src[j] != ':'))
                syntaxError("Unknown inline modifier", src);
            const bool scoped = src[j] == ':';
            if (!on.empty() || !off.empty()) {
                out += "(?" + on + (off.empty() ? "" : "-" + off) + (scoped ? ":" : ")");
            } else if (scoped) {
                out += "(?:";
            }
            if (scoped) leading = false;
            i = j + 1;
            continue;
        }
        const std::size_t len = utf8Length(static_cast<unsigned char>(c));
        out.append(src, i, len);
        i += len;
        leading = false;
    }
    return out;
}

namespace {

std::string pcreMessage(int code) {
    PCRE2_UCHAR buf[256];
    if (pcre2_get_error_message(code, buf, sizeof buf) < 0) return "error " + std::to_string(code);
    return reinterpret_cast<const char*>(buf);
}

pcre2_code* compileCode(const CompiledPattern& cp, const std::string& pcre, unsigned extraOptions,
                        bool unixLines, std::uint32_t anchoring) {
    pcre2_compile_context* cctx = pcre2_compile_context_create(nullptr);
    if (!cctx) throw std::bad_alloc();
    pcre2_set_newline(cctx, unixLines ? PCRE2_NEWLINE_LF : PCRE2_NEWLINE_ANYCRLF);
    int err = 0;
    PCRE2_SIZE offset = 0;
    pcre2_code* code = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pcre.data()), pcre.size(),
                                     PCRE2_UTF | PCRE2_MATCH_INVALID_UTF | extraOptions | anchoring,
                                     &err, &offset, cctx);
    pcre2_compile_context_free(cctx);
    if (!code)
        throw PatternSyntaxError(pcreMessage(err) + " near index " + std::to_string(offset) +
                                 ": " + cp.source);
    return code;
}

// Compiles cp's source as PCRE2 code, anchored as `anchoring` says, and
// JIT-compiles it when the library can (else the interpreter runs it).
pcre2_code* buildCode(const CompiledPattern& cp, std::uint32_t anchoring, bool& jit) {
    unsigned extraOptions = 0;
    bool unixLines = false;
    const std::string pcre = translatePattern(cp.source, extraOptions, unixLines);
    pcre2_code* code = compileCode(cp, pcre, extraOptions, unixLines, anchoring);
    jit = pcre2_jit_compile(code, PCRE2_JIT_COMPLETE) == 0;
    return code;
}

// ---------------------------------------------------------------- matching

// The match limit is g_matchLimit (kRegexMatchLimit unless a test lowers
// it). The heap limit bounds the interpreter, used only where the JIT is
// not available.
constexpr std::uint32_t kHeapLimitKiB = 1u << 20;  // 1 GiB
std::atomic<std::uint32_t> g_matchLimit{kRegexMatchLimit};

// The calling thread's match data, match context and JIT stack.
struct ThreadMatchState {
    pcre2_match_context* context = nullptr;
    pcre2_jit_stack* jitStack = nullptr;
    pcre2_match_data* data = nullptr;
    std::uint32_t pairs = 0;  // ovector pairs of `data`

    ThreadMatchState() {
        context = pcre2_match_context_create(nullptr);
        jitStack = pcre2_jit_stack_create(32u << 10, kJitStackMaxBytes, nullptr);
        if (!context || !jitStack) {
            release();
            throw std::bad_alloc();
        }
        pcre2_jit_stack_assign(context, nullptr, jitStack);
        pcre2_set_heap_limit(context, kHeapLimitKiB);
    }
    ~ThreadMatchState() { release(); }
    ThreadMatchState(const ThreadMatchState&) = delete;
    ThreadMatchState& operator=(const ThreadMatchState&) = delete;

    void release() {
        if (data) pcre2_match_data_free(data);
        if (jitStack) pcre2_jit_stack_free(jitStack);
        if (context) pcre2_match_context_free(context);
        data = nullptr;
        jitStack = nullptr;
        context = nullptr;
    }

    // Match data with room for `groups` groups and the whole match.
    pcre2_match_data* dataFor(unsigned groups) {
        if (!data || pairs < groups + 1) {
            const std::uint32_t want = std::max<std::uint32_t>(groups + 1, 16);
            pcre2_match_data* fresh = pcre2_match_data_create(want, nullptr);
            if (!fresh) throw std::bad_alloc();
            if (data) pcre2_match_data_free(data);
            data = fresh;
            pairs = want;
        }
        return data;
    }
};

ThreadMatchState& threadMatchState() {
    thread_local ThreadMatchState state;
    return state;
}

[[noreturn]] void engineGaveUp(const CompiledPattern* cp, int rc) {
    const char* limit = rc == PCRE2_ERROR_JIT_STACKLIMIT ? "JIT stack limit"
                      : rc == PCRE2_ERROR_MATCHLIMIT     ? "match limit"
                      : rc == PCRE2_ERROR_DEPTHLIMIT     ? "depth limit"
                                                         : "heap limit";
    throw StackOverflowError("StackOverflowError: matching the pattern " + cp->source +
                             " went past the regular-expression engine's " + limit + " (" +
                             pcreMessage(rc) + ")");
}

bool runMatch(const CompiledPattern* cp, const pcre2_code* code, std::string_view subject,
              std::size_t from, Match& m) {
    ThreadMatchState& st = threadMatchState();
    pcre2_match_data* md = st.dataFor(cp->groups);
    pcre2_set_match_limit(st.context, g_matchLimit.load(std::memory_order_relaxed));
    const int rc = pcre2_match(code, reinterpret_cast<PCRE2_SPTR>(subject.data()), subject.size(),
                               from, 0, md, st.context);
    if (rc == PCRE2_ERROR_NOMATCH) return false;
    if (rc < 0) {
        if (rc == PCRE2_ERROR_JIT_STACKLIMIT || rc == PCRE2_ERROR_MATCHLIMIT ||
            rc == PCRE2_ERROR_DEPTHLIMIT || rc == PCRE2_ERROR_HEAPLIMIT)
            engineGaveUp(cp, rc);
        if (rc == PCRE2_ERROR_NOMEMORY)
            throw std::runtime_error("OutOfMemoryError: matching the pattern " + cp->source +
                                     ": " + pcreMessage(rc));
        throw std::runtime_error("matching the pattern " + cp->source + " failed: " +
                                 pcreMessage(rc));
    }
    const PCRE2_SIZE* ov = pcre2_get_ovector_pointer(md);
    const std::size_t n = 2 * (static_cast<std::size_t>(cp->groups) + 1);
    m.offsets.resize(n);
    // Groups past the highest one that took part (rc - 1) are unset.
    const std::size_t set = 2 * static_cast<std::size_t>(rc);
    for (std::size_t k = 0; k < n; ++k)
        m.offsets[k] = k < set && ov[k] != PCRE2_UNSET ? static_cast<std::size_t>(ov[k]) : Match::kUnset;
    return true;
}

} // namespace

bool regexSearch(const CompiledPattern* cp, std::string_view subject, std::size_t from, Match& m) {
    if (from > subject.size()) return false;
    return runMatch(cp, static_cast<const pcre2_code*>(cp->code), subject, from, m);
}

bool regexMatchWhole(const CompiledPattern* cp, std::string_view subject, Match& m) {
    std::call_once(cp->wholeOnce, [cp] {
        bool jit = false;
        cp->wholeCode = buildCode(*cp, PCRE2_ANCHORED | PCRE2_ENDANCHORED, jit);
    });
    return runMatch(cp, static_cast<const pcre2_code*>(cp->wholeCode), subject, 0, m);
}

void setRegexMatchLimit(std::uint32_t limit) {
    g_matchLimit.store(limit, std::memory_order_relaxed);
}

std::size_t nextCharacter(std::string_view s, std::size_t pos) {
    if (pos >= s.size()) return pos + 1;
    const std::size_t len = utf8Length(static_cast<unsigned char>(s[pos]));
    if (pos + len > s.size()) return pos + 1;
    for (std::size_t k = 1; k < len; ++k)
        if ((static_cast<unsigned char>(s[pos + k]) & 0xC0) != 0x80) return pos + 1;
    return pos + len;
}

const proto::ProtoObject* initRegex(proto::ProtoContext* ctx) {
    g_patternPrototype = ctx->space->objectPrototype->newChild(ctx, /*isMutable=*/false);
    g_matcherPrototype = ctx->space->objectPrototype->newChild(ctx, /*isMutable=*/false);
    return ctx->newList()
        ->appendLast(ctx, g_patternPrototype)
        ->appendLast(ctx, g_matcherPrototype)
        ->asObject(ctx);
}

const proto::ProtoObject* matcherPrototype() { return g_matcherPrototype; }

const CompiledPattern* compilePattern(const std::string& source) {
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    auto it = g_cache.find(source);
    if (it != g_cache.end()) return it->second.get();
    auto compiled = std::make_unique<CompiledPattern>();
    compiled->source = source;
    auto* code = buildCode(*compiled, 0, compiled->jit);
    compiled->code = code;
    std::uint32_t captures = 0;
    pcre2_pattern_info(code, PCRE2_INFO_CAPTURECOUNT, &captures);
    compiled->groups = captures;
    const CompiledPattern* result = compiled.get();
    g_cache.emplace(source, std::move(compiled));
    return result;
}

const proto::ProtoObject* makePattern(proto::ProtoContext* ctx, const std::string& source) {
    const CompiledPattern* compiled = compilePattern(source);
    CallerSlots scope(ctx, 1);
    scope.set(0, g_patternPrototype->newChild(ctx, /*isMutable=*/false)
        ->setAttribute(ctx, sourceKey(ctx), ctx->fromUTF8String(source.c_str())));
    return scope.get(0)->setAttribute(ctx, regexKey(ctx),
        ctx->fromExternalPointer(const_cast<CompiledPattern*>(compiled)));
}

bool isPattern(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
    return g_patternPrototype && isObjectCell(v) && v->getPrototype(ctx) == g_patternPrototype;
}

bool isMatcher(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
    return g_matcherPrototype && isObjectCell(v) && v->getPrototype(ctx) == g_matcherPrototype;
}

const CompiledPattern* patternOf(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
    const proto::ProtoObject* p = v->getOwnAttributeDirect(ctx, regexKey(ctx));
    return static_cast<const CompiledPattern*>(p->asExternalPointer(ctx)->getPointer(ctx));
}

std::string patternSource(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
    return reinterpret_cast<const proto::ProtoString*>(v->getOwnAttributeDirect(ctx, sourceKey(ctx)))
        ->toStdString(ctx);
}

} // namespace protoClojure
