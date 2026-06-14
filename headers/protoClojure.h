/*
 * protoClojure — public API
 *
 * The header an embedder includes to talk to the runtime. v0.0.x ships a
 * deliberately tiny surface; v0.1 will grow it as Phase 1 of the roadmap
 * lands actual interpreter functionality.
 *
 * See docs/superpowers/specs/2026-06-14-phase-1-bootstrap-interpreter.md
 * for what comes next.
 */
#pragma once

namespace protoClojure {

// Major.minor.patch.  Matches CMake's project() version.
constexpr int kVersionMajor = 0;
constexpr int kVersionMinor = 0;
constexpr int kVersionPatch = 1;

const char* versionString();

} // namespace protoClojure
