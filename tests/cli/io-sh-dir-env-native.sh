#!/usr/bin/env bash
#
# CLI check: `sh` sets a program's working directory (:dir) and environment
# (:env) itself, through protoIO's process API, with no shell or `env`
# program in between. protoclj runs with a PATH that holds only the system
# directory: no `sh`, no `env` (on Windows, none of Git for Windows' tools;
# elsewhere, not even /bin or /usr/bin), so a route through `sh -c 'cd ...'`
# or `env -i` fails. The programs are named so they are found without PATH.
# Registered as the `cli/io-sh-dir-env-native` ctest case by
# tests/CMakeLists.txt.
#
# Usage: io-sh-dir-env-native.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: io-sh-dir-env-native.sh <protoclj>}"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir "$work/sub dir"

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        # cmd.exe is found in the system directory, which protoIO searches
        # whatever PATH holds.
        minimal_path="$(cygpath -u "$SYSTEMROOT")/System32"
        native_dir=$(cygpath -m "$work/sub dir")
        dir_prog='(sh "cmd" "/c" "cd" :dir d)'
        env_prog='(sh "cmd" "/c" "echo" "%FOO%" :env {"FOO" "bar"})'
        expected_env="bar"
        ;;
    *)
        minimal_path=/nonexistent-protoclj-path
        native_dir="$work/sub dir"
        dir_prog='(sh "/bin/pwd" :dir d)'
        # The whole environment is the one given: env prints exactly it.
        env_prog='(sh "/usr/bin/env" :env {"FOO" "bar"})'
        expected_env="FOO=bar"
        ;;
esac

run() {  # run <name> <source>; leaves stdout in $out
    printf '%s\n' "$2" >"$work/$1.clj"
    out=$(cd "$work" && timeout 90s env PATH="$minimal_path" "$PROTOCLJ" "$1.clj" 2>"$work/err")
    rc=$?
    out=${out//$'\r'/}
    if [[ $rc -ne 0 ]]; then
        echo "FAIL: $1: rc=$rc"
        echo "stdout:"; sed 's/^/  /' <<<"$out"
        echo "stderr:"; sed 's/^/  /' "$work/err"
        exit 1
    fi
}

run dir "(def d \"$native_dir\") (def r $dir_prog) (println (str (:exit r) \" \" (:out r) (:err r)))"
# The directory the program printed is the one given (compared by its last
# component: the temporary directory may be reached through a symbolic link
# or an 8.3 name).
if [[ "$out" != 0\ *"sub dir"* ]]; then
    echo "FAIL: :dir: expected exit 0 and a path ending in 'sub dir', got: $out"
    exit 1
fi

run env "(def r $env_prog) (println (str (:exit r) \" \" (:out r) (:err r)))"
if [[ "$(printf '%s' "$out" | sed 's/[[:space:]]*$//')" != "0 $expected_env" ]]; then
    echo "FAIL: :env: expected '0 $expected_env', got: $out"
    exit 1
fi

# A missing directory is an IOException, as the JVM's.
run missing "(println (try (sh \"cmd\" :dir \"$native_dir/missing\") (catch IOException e \"IOException\")))"
if [[ "$out" != "IOException" ]]; then
    echo "FAIL: missing :dir: expected IOException, got: $out"
    exit 1
fi
echo OK
