#include "Regex.h"
#include "StackGuard.h"

#include "protoCore.h"

#include <cctype>
#include <memory>
#include <mutex>
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

// The Java pattern `src` as an ECMAScript one (see Regex.h), with its
// options. Throws PatternSyntaxError for a flag the engine cannot honour.
std::string translate(const std::string& src, std::regex::flag_type& flags) {
    flags = std::regex::ECMAScript;
    std::size_t i = 0;
    bool dotAll = false, multiline = false;
    // Leading inline flags, possibly several groups: (?i)(?s)...
    while (src.compare(i, 2, "(?") == 0) {
        std::size_t j = i + 2;
        std::string letters;
        while (j < src.size() && std::isalpha(static_cast<unsigned char>(src[j]))) letters += src[j++];
        if (letters.empty() || j >= src.size() || src[j] != ')') break;
        for (char c : letters) {
            switch (c) {
                case 'i': flags |= std::regex::icase; break;
                case 'm': multiline = true; break;
                case 's': dotAll = true; break;
                case 'u': case 'U': break;  // Unicode case folding: bytes here (D45)
                default:
                    throw PatternSyntaxError(std::string("Unsupported inline flag (?") + c +
                                             ") in pattern: " + src);
            }
        }
        i = j + 1;
    }
    // std::regex::multiline is C++17; MSVC's standard library does not
    // provide it (C2039 on the Windows runners), so there (?m) is accepted
    // and has no effect of its own (STATUS.md, known issues).
#if !defined(_MSVC_STL_VERSION)
    if (multiline) flags |= std::regex::multiline;
#endif
    std::string out;
    out.reserve(src.size() + 8);
    bool inClass = false;
    for (; i < src.size(); ++i) {
        const char c = src[i];
        if (c == '\\' && i + 1 < src.size()) {
            const char n = src[i + 1];
            if (n == 'Q') {
                // \Q...\E: every character up to \E (or the end) literally.
                std::size_t end = src.find("\\E", i + 2);
                const std::string quoted = src.substr(i + 2, end == std::string::npos
                                                                 ? std::string::npos : end - i - 2);
                for (char q : quoted) {
                    if (std::string("\\^$.|?*+()[]{}/-").find(q) != std::string::npos) out += '\\';
                    out += q;
                }
                i = (end == std::string::npos) ? src.size() : end + 1;
                continue;
            }
            if (!inClass && !multiline && n == 'A') { out += '^'; ++i; continue; }
            if (!inClass && !multiline && (n == 'z' || n == 'Z')) { out += '$'; ++i; continue; }
            out += c;
            out += n;
            ++i;
            continue;
        }
        if (inClass) {
            if (c == ']') inClass = false;
            out += c;
            continue;
        }
        if (c == '[') {
            inClass = true;
            out += c;
            // A `]` right after `[` or `[^` is a literal member.
            if (i + 1 < src.size() && src[i + 1] == '^') { out += '^'; ++i; }
            if (i + 1 < src.size() && src[i + 1] == ']') { out += "\\]"; ++i; }
            continue;
        }
        if (c == '(' && src.compare(i, 3, "(?<") == 0 && i + 3 < src.size() &&
            src[i + 3] != '=' && src[i + 3] != '!') {
            // (?<name>X): a numbered group.
            const std::size_t close = src.find('>', i + 3);
            if (close == std::string::npos)
                throw PatternSyntaxError("Unclosed group name in pattern: " + src);
            out += '(';
            i = close;
            continue;
        }
        if (c == '.' && dotAll) { out += "[\\s\\S]"; continue; }
        out += c;
    }
    return out;
}

unsigned countGroups(const std::regex& re) {
    return static_cast<unsigned>(re.mark_count());
}

} // namespace

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
    std::regex::flag_type flags{};
    const std::string ecma = translate(source, flags);
    auto compiled = std::make_unique<CompiledPattern>();
    compiled->source = source;
#if defined(_LIBCPP_VERSION)
    compiled->recursive = false;  // libc++ backtracks on the heap (Regex.h)
#else
    compiled->recursive = true;
#endif
    try {
#if defined(__GLIBCXX__)
        try {
            compiled->regex = std::regex(ecma, flags | std::regex_constants::__polynomial);
            compiled->recursive = false;
        } catch (const std::regex_error&) {
            // Back-references need the backtracking executor; a genuine
            // syntax error is reported by the compilation below.
            compiled->regex = std::regex(ecma, flags);
        }
#else
        compiled->regex = std::regex(ecma, flags);
#endif
    } catch (const std::regex_error& e) {
        throw PatternSyntaxError(std::string(e.what()) + ": " + source);
    }
    compiled->groups = countGroups(compiled->regex);
    const CompiledPattern* result = compiled.get();
    g_cache.emplace(source, std::move(compiled));
    return result;
}

std::regex_constants::match_flag_type continuationFlags(const CompiledPattern* cp,
                                                       const std::string& subject,
                                                       std::size_t pos) {
    if (pos == 0 || pos > subject.size()) return std::regex_constants::match_default;
#if defined(_LIBCPP_VERSION)
    // libc++ clears match_not_bol under match_prev_avail, as the standard
    // says, and then takes the first position for the beginning of the
    // input anyway, so `^` matched at every continued search. Without
    // match_prev_avail it honours match_not_bol and match_not_bow, which
    // say what the byte before `pos` would have said: not the beginning of
    // a line (unless a multi-line `^` follows a line terminator) and not the
    // beginning of a word when it is a word byte. The one case this cannot
    // express is `\b` right after a word byte before a non-word byte
    // (STATUS.md, known issues).
    const unsigned char prev = static_cast<unsigned char>(subject[pos - 1]);
    const bool afterLineEnd = prev == '\n' || prev == '\r';
    const bool multiline = (cp->regex.flags() & std::regex::multiline) != 0;
    auto flags = std::regex_constants::match_default;
    if (!(multiline && afterLineEnd)) flags |= std::regex_constants::match_not_bol;
    if (prev == '_' || std::isalnum(prev)) flags |= std::regex_constants::match_not_bow;
    return flags;
#else
    (void)cp;
    return std::regex_constants::match_prev_avail;
#endif
}

void guardRecursiveMatch(const CompiledPattern* cp, std::size_t subjectBytes) {
    if (!cp->recursive) return;
    checkNativeStack();  // computes this thread's limit on its first use
    const char probe = 0;
    const auto here = reinterpret_cast<std::uintptr_t>(&probe);
    const std::uintptr_t limit = detail::tl_stackLimit;
    const std::size_t free = here > limit ? static_cast<std::size_t>(here - limit) : 0;
    if (subjectBytes > free / 1024)
        throw StackOverflowError("StackOverflowError: matching the pattern " + cp->source +
                                 " (it has back-references, so it backtracks on the native "
                                 "stack) against " + std::to_string(subjectBytes) +
                                 " bytes could exhaust the thread stack");
}

const proto::ProtoObject* makePattern(proto::ProtoContext* ctx, const std::string& source) {
    const CompiledPattern* compiled = compilePattern(source);
    proto::ProtoContext scope(ctx->space, ctx);
    scope.resizeAutomaticLocals(1);
    scope.setAutomaticLocal(0, g_patternPrototype->newChild(&scope, /*isMutable=*/false)
        ->setAttribute(&scope, sourceKey(&scope), scope.fromUTF8String(source.c_str())));
    return scope.getAutomaticLocal(0)->setAttribute(&scope, regexKey(&scope),
        scope.fromExternalPointer(const_cast<CompiledPattern*>(compiled)));
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
