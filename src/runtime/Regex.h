/*
 * Regex — protoClojure's regular expressions (docs/DESIGN.md §10.2, M25).
 *
 * Engine: the C++ standard library's std::regex in its ECMAScript grammar,
 * over the UTF-8 bytes of the subject (deviation D45 lists the dialect
 * differences against java.util.regex). A pattern written for Java is first
 * translated where the two dialects have an equivalent:
 *   - leading inline flags `(?i)`, `(?m)`, `(?s)` (and combinations such as
 *     `(?im)`) become the engine's icase and multiline options, `s` by
 *     rewriting every `.` outside a character class as `[\s\S]`; `(?u)` and
 *     `(?U)` are accepted and change nothing;
 *   - a named group `(?<name>X)` becomes the numbered group `(X)` (it keeps
 *     its number, which is what re-groups and re-find report);
 *   - `\Q...\E` becomes the quoted characters, each escaped;
 *   - `\A` becomes `^` and `\z` / `\Z` become `$` when the pattern is not
 *     multiline.
 * Anything else the engine rejects (lookbehind, possessive quantifiers,
 * atomic groups, `\p{...}` classes, flags in the middle of a pattern, `x`)
 * is a PatternSyntaxError naming the pattern.
 *
 * Values. A pattern is an immutable object whose prototype is the pattern
 * prototype; its own attribute `__pattern__` holds the source as written
 * and `__regex__` an external pointer to the compiled form. Compiled forms
 * live in a process-wide cache keyed by the source and are never freed, so
 * a pattern literal inside a loop is compiled once and `re-pattern` of a
 * string seen before is a lookup (D45: a program that builds unboundedly
 * many distinct patterns grows by one compiled pattern each). The cache is
 * guarded by a mutex held only around the C++ map and the compilation, never
 * across a protoCore allocation. Matching on a compiled pattern is
 * read-only and safe from any thread.
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

#include <regex>
#include <stdexcept>
#include <string>

namespace proto {
class ProtoContext;
class ProtoObject;
}

namespace protoClojure {

// A pattern the engine cannot compile; the message says why, Java-style:
// "Unclosed group near index 3: (ab" is not attempted, the engine's own
// explanation is given with the pattern.
struct PatternSyntaxError : std::runtime_error {
    explicit PatternSyntaxError(const std::string& m) : std::runtime_error(m) {}
};

struct CompiledPattern {
    std::string source;   // as written
    std::regex  regex;
    unsigned    groups;   // capturing groups
    // True when matching recurses on the native stack once per subject
    // character: the engine's backtracking executor. With libstdc++ a
    // pattern without back-references runs on its breadth-first executor
    // (the `__polynomial` option), whose native depth does not grow with the
    // subject; a pattern with back-references runs on the recursive one.
    // libc++ (macOS) backtracks on a heap vector of states, never on the
    // native stack (basic_regex::__match_at_start_ecma), so no pattern is
    // recursive there. Every other standard library (MSVC) counts as
    // recursive.
    bool        recursive;
};

// The match flags for a search that starts at byte `pos` of `subject` and
// continues an earlier one (re-find on a matcher, re-seq, replace, split):
// the text before `pos` is context, as for Java's Matcher.find. That is
// match_prev_avail; with libc++ also match_not_bol, unless `^` may truly
// match there (a multi-line pattern right after a line terminator), because
// libc++ takes the first position of every search for the beginning of the
// input whatever match_prev_avail says, so `^` matched at every step.
std::regex_constants::match_flag_type continuationFlags(const CompiledPattern* cp,
                                                       const std::string& subject,
                                                       std::size_t pos);

// Calls `f(m)` for each successive match of `cp` in `s`, as
// std::sregex_iterator walks them (after an empty match, a non-empty match
// at the same place first, else the search moves one byte on), but with
// continuationFlags for every search after the first. The positions in `m`
// are relative to where that search started: use m[0].first - s.begin().
// `f` returns false to stop.
template <typename F>
void forEachMatch(const std::string& s, const CompiledPattern* cp, F&& f) {
    namespace rc = std::regex_constants;
    std::smatch m;
    auto search = [&](std::size_t from, rc::match_flag_type extra) {
        return std::regex_search(s.cbegin() + static_cast<std::ptrdiff_t>(from), s.cend(), m,
                                 cp->regex, continuationFlags(cp, s, from) | extra);
    };
    if (!search(0, rc::match_default)) return;
    for (;;) {
        if (!f(static_cast<const std::smatch&>(m))) return;
        const std::size_t start = static_cast<std::size_t>(m[0].first - s.cbegin());
        const std::size_t end = static_cast<std::size_t>(m[0].second - s.cbegin());
        if (end > start) {
            if (!search(end, rc::match_default)) return;
            continue;
        }
        if (end >= s.size()) return;
        if (search(end, rc::match_not_null | rc::match_continuous)) continue;
        if (!search(end + 1, rc::match_default)) return;
    }
}

// Raises StackOverflowError, before matching, when a recursive pattern
// (above) on a subject of `subjectBytes` could exhaust the thread's native
// stack; the estimate is 1 KiB of stack per subject byte, the worst measured
// (`(a|b)*` with libstdc++, about 840 bytes).
void guardRecursiveMatch(const CompiledPattern* cp, std::size_t subjectBytes);

// Creates the pattern and matcher prototypes of this ProtoSpace. Returns
// the pattern prototype; the matcher prototype is a child-free sibling the
// caller roots through matcherPrototype().
const proto::ProtoObject* initRegex(proto::ProtoContext* ctx);
const proto::ProtoObject* matcherPrototype();

// The compiled form of `source`, from the cache or compiled now. Throws
// PatternSyntaxError.
const CompiledPattern* compilePattern(const std::string& source);

// A pattern value for `source`. Throws PatternSyntaxError.
const proto::ProtoObject* makePattern(proto::ProtoContext* ctx, const std::string& source);

bool isPattern(proto::ProtoContext* ctx, const proto::ProtoObject* v);

// The compiled form and the source of pattern `v` (`isPattern` must hold).
const CompiledPattern* patternOf(proto::ProtoContext* ctx, const proto::ProtoObject* v);
std::string patternSource(proto::ProtoContext* ctx, const proto::ProtoObject* v);

bool isMatcher(proto::ProtoContext* ctx, const proto::ProtoObject* v);

} // namespace protoClojure
