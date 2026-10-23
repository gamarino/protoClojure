#!/usr/bin/env bash
#
# CLI check: core.clj is embedded in the binary. The clojure.core macros it
# defines (src/core/core.clj) work when protoclj runs from a directory of its
# own, away from the source and build trees, as an installed protoclj (the
# deb, the tarball) does: no core.clj file is looked up at run time. Also
# checks that the core macros are there at the REPL. Registered as the
# `cli/core-embedded` ctest case by tests/CMakeLists.txt.
#
# Usage: core-embedded.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: core-embedded.sh <protoclj>}"

dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
cp "$PROTOCLJ" "$dir/protoclj"
printf '(println (when-let [x 2] (-> x inc (* 10))) (case :b :a 1 :b 2))\n' > "$dir/p.clj"
out=$(cd "$dir" && env -u PROTOCLJ_HOME ./protoclj p.clj 2>&1)
rc=$?
if [[ $rc -ne 0 || "$out" != "30 2" ]]; then
    echo "FAIL: copied binary printed '$out' (exit $rc), expected '30 2'"
    exit 1
fi
out=$(printf '%s\n' "(macroexpand '(-> a (b c)))" "(dotimes [i 2] (print i))" | (cd "$dir" && ./protoclj 2>&1))
if ! grep -qF 'user=> (b a c)' <<<"$out" || ! grep -qF '01' <<<"$out"; then
    echo "FAIL: core macros missing at the REPL"
    sed 's/^/  /' <<<"$out"
    exit 1
fi
echo OK
