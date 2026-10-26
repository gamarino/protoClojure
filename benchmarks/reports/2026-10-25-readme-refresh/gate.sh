#!/usr/bin/env bash
# Waits before a measurement round, then logs the machine state.
#
#   1. $IDLE_GATE, if set (a command that waits until no compiler, make, ninja,
#      cmake or ctest runs);
#   2. until no other protoCore runtime or benchmark process is running
#      (protojs, protopy, protost, protoscala, protoclj, bb, ...) and no
#      process outside the desktop session uses more than half a CPU,
#      checked every 15 s, at most GATE_MAX_WAIT seconds (default 7200);
#   3. appends one line to $GATE_LOG: the label, the time, the load average,
#      the seconds waited, and whether the wait timed out.
#
# The desktop session (browser, compositor) keeps the load average around 6
# on this machine; it is recorded, not waited for.
#
# Usage: gate.sh LABEL
set -u
label=${1:-round}
log=${GATE_LOG:-/dev/stderr}
max=${GATE_MAX_WAIT:-7200}
[[ -n "${IDLE_GATE:-}" ]] && $IDLE_GATE
stray() {
    # Other runtimes and benchmark binaries, by process name.
    pgrep -a -x 'protojs|protopy|protopython|protost|protoscala|protoclj|protoclj_.*|bb|evalgrow' || true
    # Any other process above 50 % of a CPU over a 2-second sample, the
    # desktop session excepted.
    LC_ALL=C top -b -n 2 -d 2 -w 200 | awk '/^top -/ { n++ } n == 2 && $1 ~ /^[0-9]+$/ && $9 + 0 > 50 &&
        $12 !~ /^(chrome|gnome-shell|Xwayland|top|pipewire|wireplumber|pulseaudio|mutter|kworker)/ { print $1, $9, $12 }'
}
waited=0
while [[ -n "$(stray)" ]] && (( waited < max )); do
    sleep 15; waited=$((waited + 15))
done
note=""
(( waited >= max )) && note=", TIMED OUT with: $(stray | head -3 | cut -c1-120 | tr '\n' ';')"
echo "$label $(date -u +%T) load $(cut -d' ' -f1-3 /proc/loadavg) waited ${waited}s$note" >> "$log"
