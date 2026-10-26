// Times one search per line of an input file with several regular-expression
// engines, outside any runtime: the cost of the engine alone. Each variant
// prints its match count and total time, so a silent failure shows.
//
//   g++ -O2 -std=c++20 regex_engines.cpp -lpcre2-8 -o regex_engines
//   regex_engines FILE
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

static const char* kPattern = "ERROR .* latency=(\\d{4,})ms";

template <typename F>
static void timeIt(const char* name, const std::vector<std::string>& lines, F&& f) {
    const auto t0 = std::chrono::steady_clock::now();
    long matches = 0;
    for (const std::string& l : lines) matches += f(l) ? 1 : 0;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%-40s matches %ld  %8.1f ms  %6.0f ns/line\n", name, matches, ms, ms * 1e6 / lines.size());
}

int main(int argc, char** argv) {
    std::ifstream in(argv[1]);
    std::vector<std::string> lines;
    for (std::string l; std::getline(in, l);) lines.push_back(std::move(l));
    std::printf("lines %zu\n", lines.size());

    const std::regex poly(kPattern, std::regex::ECMAScript | std::regex_constants::__polynomial);
    const std::regex dfs(kPattern, std::regex::ECMAScript);
    const std::regex lit("ERROR", std::regex::ECMAScript | std::regex_constants::__polynomial);
    std::smatch m;
    timeIt("std::regex __polynomial (current)", lines, [&](const std::string& s) { return std::regex_search(s, m, poly); });
    timeIt("std::regex __polynomial, literal ERROR", lines, [&](const std::string& s) { return std::regex_search(s, m, lit); });
    timeIt("std::regex backtracking (default)", lines, [&](const std::string& s) { return std::regex_search(s, m, dfs); });
    timeIt("memmem(\"ERROR \") + __polynomial", lines, [&](const std::string& s) {
        return memmem(s.data(), s.size(), "ERROR ", 6) && std::regex_search(s, m, poly); });

    int err; PCRE2_SIZE off;
    pcre2_code* re = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(kPattern), PCRE2_ZERO_TERMINATED, 0, &err, &off, nullptr);
    pcre2_match_data* md = pcre2_match_data_create_from_pattern(re, nullptr);
    timeIt("PCRE2 interpreter", lines, [&](const std::string& s) {
        return pcre2_match(re, reinterpret_cast<PCRE2_SPTR>(s.data()), s.size(), 0, 0, md, nullptr) >= 0; });
    if (pcre2_jit_compile(re, PCRE2_JIT_COMPLETE) == 0)
        timeIt("PCRE2 JIT", lines, [&](const std::string& s) {
            return pcre2_match(re, reinterpret_cast<PCRE2_SPTR>(s.data()), s.size(), 0, 0, md, nullptr) >= 0; });
    return 0;
}
