// Times, per input line, the protoCore operations protoClojure's grep does
// for every line outside the interpreter: building the line string
// (fromUTF8Buffer), converting it back to std::string (toStdString, as every
// string primitive and re-find do), and entering and leaving an
// UnmanagedScope (as every protoIO call through IO.cpp's blocking() does).
// Each step prints a checksum of the work so a silent failure shows.
//
//   g++ -O2 -std=c++20 -I$PREFIX/include protocore_line_ops.cpp -L$PREFIX/lib -lprotoCore -o protocore_line_ops
//   protocore_line_ops FILE
#include "protoCore.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;
static double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

int main(int argc, char** argv) {
    std::ifstream in(argv[1]);
    std::vector<std::string> lines;
    for (std::string l; std::getline(in, l);) lines.push_back(std::move(l));
    const double n = static_cast<double>(lines.size());

    proto::ProtoSpace space;
    proto::ProtoContext* ctx = space.rootContext;

    // 1. fromUTF8Buffer + toStdString, line by line, in a fresh context per
    //    batch so the young chain is handed back as protoClojure's frames do.
    std::size_t bytes = 0;
    double buildMs = 0, convMs = 0;
    const std::size_t batch = 4096;
    for (std::size_t i = 0; i < lines.size(); i += batch) {
        proto::ProtoContext frame(&space, ctx);
        std::vector<const proto::ProtoString*> strs;
        strs.reserve(batch);
        auto t0 = Clock::now();
        for (std::size_t j = i; j < std::min(lines.size(), i + batch); ++j) {
            std::uint8_t rem[4]; std::uint8_t rc = 0;
            strs.push_back(proto::ProtoString::fromUTF8Buffer(&frame,
                reinterpret_cast<const std::uint8_t*>(lines[j].data()), lines[j].size(),
                nullptr, 0, rem, &rc));
        }
        buildMs += msSince(t0);
        t0 = Clock::now();
        for (const proto::ProtoString* s : strs) bytes += s->toStdString(&frame).size();
        convMs += msSince(t0);
    }
    std::printf("fromUTF8Buffer   %8.1f ms  %6.0f ns/line\n", buildMs, buildMs * 1e6 / n);
    std::printf("toStdString      %8.1f ms  %6.0f ns/line  (bytes %zu)\n", convMs, convMs * 1e6 / n, bytes);

    // 2. An UnmanagedScope per line, with nothing inside.
    auto t0 = Clock::now();
    volatile std::size_t sink = 0;
    for (const std::string& l : lines) {
        proto::ProtoContext::UnmanagedScope out(ctx);
        sink += l.size();
    }
    const double scopeMs = msSince(t0);
    std::printf("UnmanagedScope   %8.1f ms  %6.0f ns/line  (bytes %zu)\n", scopeMs, scopeMs * 1e6 / n,
                static_cast<std::size_t>(sink));
    return 0;
}
