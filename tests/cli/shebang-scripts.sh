#!/usr/bin/env bash
#
# CLI check: a script whose first line is `#!/usr/bin/env protoclj` runs as a
# command. `#!` is a line comment (as in Clojure), read errors keep the file's
# real line numbers, and the shipped filters (examples/13-filter-grep.clj,
# examples/14-filter-count.clj) chain in pipelines with exit statuses intact.
# Registered as the `cli/shebang-scripts` ctest case by tests/CMakeLists.txt.
#
# Usage: shebang-scripts.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: shebang-scripts.sh <protoclj>}"
SRC="$(cd "$(dirname "$0")/../.." && pwd)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f:"; sed 's/^/  /' "$f"; done; exit 1; }

# Through protoclj: the interpreter line is a comment; a read error on line 3
# is reported on line 3.
printf '#!/usr/bin/env protoclj\n(println (+ 3 4))\n' >"$work/ok.clj"
out=$(timeout 90s "$PROTOCLJ" "$work/ok.clj" 2>"$work/err") || fail "plain run" "$work/err"
[[ "$out" == "7" ]] || fail "plain run printed '$out'" "$work/err"
printf '#!/usr/bin/env protoclj\n(println 1)\n(println #"x")\n' >"$work/bad.clj"
timeout 90s "$PROTOCLJ" "$work/bad.clj" >/dev/null 2>"$work/err"
grep -qF "bad.clj:3:" "$work/err" || fail "read error does not name line 3" "$work/err"

# Interpreter lines are a kernel feature Windows does not have; there only
# the half above is checked.
if command -v cygpath >/dev/null 2>&1; then echo "PASS (no #! execution on Windows)"; exit 0; fi

# As commands: `env` must find this build's protoclj first on PATH.
mkdir "$work/bin"; ln -s "$PROTOCLJ" "$work/bin/protoclj"
export PATH="$work/bin:$PATH"
cp "$SRC/examples/13-filter-grep.clj" "$SRC/examples/14-filter-count.clj" "$work/"
chmod +x "$work/13-filter-grep.clj" "$work/14-filter-count.clj"
cd "$work"

out=$(printf 'pump-1 ok\npump-2 ALARM\npump-3 ALARM\n' \
      | timeout 90s ./13-filter-grep.clj ALARM | timeout 90s ./14-filter-count.clj 2>"$work/err")
[[ "$out" == "2" ]] || fail "grep | count printed '$out'" "$work/err"
out=$(printf 'pump-1 ok\npump-2 ALARM\n' | timeout 90s ./13-filter-grep.clj ALARM)
[[ "$out" == "pump-2 ALARM" ]] || fail "grep printed '$out'"

# Exit statuses reach the shell: 1 when nothing matched, 2 on a usage error.
printf 'pump-1 ok\n' | timeout 90s ./13-filter-grep.clj ALARM >/dev/null; rc=$?
[[ $rc -eq 1 ]] || fail "grep without a match exited $rc, not 1"
timeout 90s ./13-filter-grep.clj </dev/null >"$work/out"; rc=$?
[[ $rc -eq 2 ]] && grep -qF "usage:" "$work/out" || fail "usage exit $rc" "$work/out"

# A filter stops when its reader goes away (head), instead of running on.
out=$(seq 1 200000 | timeout 90s ./13-filter-grep.clj 1 | head -n 2 | tr '\n' ' ')
[[ "$out" == "1 10 " ]] || fail "| head printed '$out'"

echo PASS
