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
    // subject; a pattern with back-references, and every pattern with
    // another standard library, runs on the backtracking one.
    bool        recursive;
};

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
