/*
 * Regex — protoClojure's regular expressions (docs/DESIGN.md §10.2, M25,
 * M48).
 *
 * Engine: PCRE2 (8-bit code units, UTF mode, JIT-compiled when the library
 * has a JIT), the same engine on every platform. Clojure's patterns are
 * java.util.regex patterns, and PCRE2's dialect is close to Java's: inline
 * flags anywhere in the pattern, named groups and `\k<name>`, lookbehind,
 * possessive quantifiers, atomic groups, `\Q...\E`, `\A` / `\z` / `\Z`,
 * `\p{...}` properties, `$` before a final line terminator. A Java pattern
 * is translated only where the two dialects differ (Regex.cpp, translate):
 *   - the flags `u` (Unicode case, which PCRE2 always applies in UTF mode)
 *     is dropped; `d` (UNIX_LINES) and `U` (UNICODE_CHARACTER_CLASS) are
 *     honoured at the start of the pattern and refused elsewhere; `x`
 *     becomes PCRE2's `xx`, which, like Java, ignores white space inside
 *     character classes as well;
 *   - `\uXXXX` (and a surrogate pair of them) and Java's `\0` octal escapes
 *     become `\x{...}`;
 *   - Java's POSIX classes `\p{Alpha}`, `\p{Punct}`, ... become PCRE2's
 *     `[[:alpha:]]`, `[[:punct:]]`, ..., and `\p{IsX}` becomes `\p{X}`;
 *   - a nested class `[a-c[x-z]]` (Java's union) is flattened; an
 *     intersection `[a-z&&[^b]]` or a negated nested class is a
 *     PatternSyntaxError, as is `\N{name}`.
 * Line terminators are LF, CR and CRLF (PCRE2_NEWLINE_ANYCRLF; Java also
 * counts U+0085, U+2028 and U+2029); with `(?d)` only LF. The differences
 * that remain are deviation D45.
 *
 * Values. A pattern is an immutable object whose prototype is the pattern
 * prototype; its own attribute `__pattern__` holds the source as written
 * and `__regex__` an external pointer to the compiled form. Compiled forms
 * live in a process-wide cache keyed by the source and are never freed, so
 * a pattern literal inside a loop is compiled once and `re-pattern` of a
 * string seen before is a lookup (D45: a program that builds unboundedly
 * many distinct patterns grows by one compiled pattern each). The cache is
 * guarded by a mutex held only around the C++ map and the compilation, never
 * across a protoCore allocation. A compiled pattern is read-only once
 * built, so matching on it is safe from any thread.
 *
 * Matching. Every match runs with the calling thread's own match data,
 * match context and JIT stack (thread_local, created on the thread's first
 * match, freed when the thread ends; no thread API is involved). They are
 * used only inside one pcre2_match call, which never calls back into
 * Clojure code: a Match copies the offsets out before a caller runs a
 * replacement fn, which may match again on the same thread. The JIT stack
 * grows on demand up to kJitStackMaxBytes. A match that goes past one of
 * the engine's limits (the JIT stack, the match limit, the interpreter's
 * heap limit) raises StackOverflowError, as Java's recursive engine
 * overflows on such a match; it is never answered as "no match".
 *
 * A matcher (`re-matcher`) is a mutable object holding a pattern, the
 * subject, the position the next find starts at and the groups of the last
 * successful find, as java.util.regex.Matcher does for re-find and
 * re-groups.
 *
 * initRegex creates both prototypes once per ProtoSpace; the caller roots
 * them (installPrimitives stores them on the globals).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace proto {
class ProtoContext;
class ProtoObject;
}

namespace protoClojure {

// A pattern the engine cannot compile; the message gives PCRE2's reason,
// the byte offset where it was found, and the pattern.
struct PatternSyntaxError : std::runtime_error {
    explicit PatternSyntaxError(const std::string& m) : std::runtime_error(m) {}
};

// The largest JIT stack a thread's matches may grow to (reserved when the
// thread first matches, committed only as a match uses it). `(a|b)*` uses
// 32 bytes of it per subject character, so it matches about two million
// characters; `(a)\1*` uses 8 and `.*` 2. Java's engine, which recurses on
// the thread's stack, overflows on far shorter subjects.
inline constexpr std::size_t kJitStackMaxBytes = 64u << 20;

// The engine's match limit: Java's engine has none, so it is high enough
// that only a runaway (exponential) match reaches it, after a few seconds.
inline constexpr std::uint32_t kRegexMatchLimit = 1000000000u;

// Sets the match limit of every later match (the unit tests lower it).
void setRegexMatchLimit(std::uint32_t limit);

struct CompiledPattern {
    std::string source;   // as written
    void*       code = nullptr;       // pcre2_code_8*: the pattern, for finds
    unsigned    groups = 0;           // capturing groups
    bool        jit = false;          // `code` is JIT-compiled
    // The pattern anchored at both ends (Matcher.matches, re-matches),
    // compiled on first use. PCRE2's JIT does not take PCRE2_ANCHORED or
    // PCRE2_ENDANCHORED at match time, so they are compile options of a
    // second code.
    mutable void*          wholeCode = nullptr;
    mutable std::once_flag wholeOnce;
};

// The byte offsets of one match and of its groups, copied out of the
// engine's match data. Group 0 is the whole match.
struct Match {
    static constexpr std::size_t kUnset = static_cast<std::size_t>(-1);
    std::vector<std::size_t> offsets;  // start and end of each group

    bool matched(unsigned g) const { return 2 * g < offsets.size() && offsets[2 * g] != kUnset; }
    std::size_t start(unsigned g = 0) const { return offsets[2 * g]; }
    std::size_t end(unsigned g = 0) const { return offsets[2 * g + 1]; }
    std::size_t length(unsigned g = 0) const { return end(g) - start(g); }
    std::string str(std::string_view subject, unsigned g = 0) const {
        return matched(g) ? std::string(subject.substr(start(g), length(g))) : std::string();
    }
};

// The first match of `cp` in `subject` that starts at byte `from` or
// later, as Java's Matcher.find(from): the text before `from` is context
// (for `^`, `\b`, lookbehind). False when there is none. Raises
// StackOverflowError when the engine gives up (see above).
bool regexSearch(const CompiledPattern* cp, std::string_view subject, std::size_t from, Match& m);

// The match of the whole of `subject`, as Java's Matcher.matches.
bool regexMatchWhole(const CompiledPattern* cp, std::string_view subject, Match& m);

// The byte offset after the character that starts at `pos` (a UTF-8
// sequence; one byte for an invalid one): where Java's Matcher.find goes
// on after an empty match.
std::size_t nextCharacter(std::string_view s, std::size_t pos);

// Calls `f(m)` for each successive match of `cp` in `s`, as repeated calls
// of Java's Matcher.find walk them: the next search starts where the match
// ended, or one character further after an empty match. `f` returns false
// to stop.
template <typename F>
void forEachMatch(std::string_view s, const CompiledPattern* cp, F&& f) {
    Match m;
    std::size_t from = 0;
    while (from <= s.size() && regexSearch(cp, s, from, m)) {
        if (!f(static_cast<const Match&>(m))) return;
        from = m.length() == 0 ? nextCharacter(s, m.end()) : m.end();
    }
}

// Creates the pattern and matcher prototypes of this ProtoSpace. Returns
// the pattern prototype; the matcher prototype is a child-free sibling the
// caller roots through matcherPrototype().
const proto::ProtoObject* initRegex(proto::ProtoContext* ctx);
const proto::ProtoObject* matcherPrototype();

// The compiled form of `source`, from the cache or compiled now. Throws
// PatternSyntaxError.
const CompiledPattern* compilePattern(const std::string& source);

// The Java pattern `source` as the PCRE2 pattern it is compiled as, with
// the compile options it needs (PCRE2_UCP for a leading `(?U)`) and whether
// it asked for UNIX_LINES (a leading `(?d)`). Throws PatternSyntaxError for
// what has no PCRE2 equivalent. Exposed for the unit tests.
std::string translatePattern(const std::string& source, unsigned& extraOptions, bool& unixLines);

// A pattern value for `source`. Throws PatternSyntaxError.
const proto::ProtoObject* makePattern(proto::ProtoContext* ctx, const std::string& source);

bool isPattern(proto::ProtoContext* ctx, const proto::ProtoObject* v);

// The compiled form and the source of pattern `v` (`isPattern` must hold).
const CompiledPattern* patternOf(proto::ProtoContext* ctx, const proto::ProtoObject* v);
std::string patternSource(proto::ProtoContext* ctx, const proto::ProtoObject* v);

bool isMatcher(proto::ProtoContext* ctx, const proto::ProtoObject* v);

} // namespace protoClojure
