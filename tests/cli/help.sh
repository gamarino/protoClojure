#!/usr/bin/env bash
#
# CLI check: `protoclj --help` prints its usage text and no internal
# development labels (session or phase names, "v0.0.x"-style milestone
# tags). Registered as the `cli/help` ctest case by tests/CMakeLists.txt.
#
# Usage: help.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: help.sh <protoclj>}"

out=$("$PROTOCLJ" --help 2>&1)
rc=$?
if [[ $rc -ne 0 ]]; then
    echo "FAIL: --help exited $rc"
    exit 1
fi
if ! grep -q "Usage:" <<<"$out"; then
    echo "FAIL: --help output has no 'Usage:' line"
    exit 1
fi
labels='Phase [0-9]|v0\.[0-9]+\.x|next milestone|v0\.1[0-9]'
if grep -qE "$labels" <<<"$out"; then
    echo "FAIL: --help shows internal milestone labels:"
    grep -nE "$labels" <<<"$out"
    exit 1
fi
echo OK
