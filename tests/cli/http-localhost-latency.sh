#!/usr/bin/env bash
#
# CLI check: a server started without a host answers on every interface, IPv6
# included, so a client of "localhost" is served at once wherever "localhost"
# names ::1 first (Windows, macOS). With a server bound to 0.0.0.0 only, each
# such connection was first refused on ::1; on Windows that cost about 0.3 s
# per request (2 s before protoIO 0.2.0). Registered as the
# `cli/http-localhost-latency` ctest case by tests/CMakeLists.txt.
#
#   1. The server answers http://[::1]:port/ (skipped where the host has no
#      IPv6 loopback, which the script reports).
#   2. Twenty requests to http://localhost:port/ take on average less than
#      100 ms more than twenty to http://127.0.0.1:port/ (the same program
#      otherwise, so process start-up cancels out). Measured: well under
#      10 ms per request on Linux; the bug cost ~310 ms per request on Windows.
#
# Usage: http-localhost-latency.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: http-localhost-latency.sh <protoclj>}"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

program() {  # program <host-in-url> <requests>
    cat <<EOF
(def s (run-server (fn [req] {:status 200 :body "ok"}) {:port 0}))
(defn hit [n] (if (> n 0) (do (http-get (str "http://$1:" (server-port s) "/")) (hit (dec n))) :done))
(hit $2)
(stop-server s)
(println "served")
EOF
}

run() {  # run <name> <host> <requests>; sets $elapsed_ms
    program "$2" "$3" >"$work/$1.clj"
    local start end
    start=$(date +%s%N)
    out=$(cd "$work" && timeout 90s "$PROTOCLJ" "$1.clj" 2>"$work/err")
    rc=$?
    end=$(date +%s%N)
    elapsed_ms=$(( (end - start) / 1000000 ))
    if [[ $rc -ne 0 || "$out" != "served" ]]; then
        echo "FAIL: $1: rc=$rc"
        echo "stdout:"; sed 's/^/  /' <<<"$out"
        echo "stderr:"; sed 's/^/  /' "$work/err"
        return 1
    fi
}

# 1. IPv6 loopback.
if run ipv6 "[::1]" 1; then
    echo "a server without a host answers [::1]"
elif grep -q "cannot listen\|Address family\|not supported\|unreachable" "$work/err"; then
    echo "SKIP part 1: no usable IPv6 loopback here"
else
    echo "FAIL: a server started without a host must answer on ::1"
    exit 1
fi

# 2. Latency of localhost against 127.0.0.1.
n=20
run ipv4 127.0.0.1 $n || exit 1
ipv4_ms=$elapsed_ms
run localhost localhost $n || exit 1
localhost_ms=$elapsed_ms
extra_per_request=$(( (localhost_ms - ipv4_ms) / n ))
echo "$n requests: 127.0.0.1 ${ipv4_ms} ms, localhost ${localhost_ms} ms (${extra_per_request} ms extra per request)"
if (( extra_per_request >= 100 )); then
    echo "FAIL: a localhost request costs ${extra_per_request} ms more than a 127.0.0.1 one (limit 100 ms)"
    exit 1
fi
echo OK
