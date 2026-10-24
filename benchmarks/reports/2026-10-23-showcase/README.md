# Showcase: protoclj and Babashka on the same scripts (2026-10-24)

`run.sh` here, 5 interleaved rounds (runtime order alternating), each round
behind protoCore's quiet gate (`QUIET_CORES=4.0`; load average 3.8-8.8 on
12 CPUs at the round starts, `out/rounds.txt`). Wall time and peak RSS from
`/usr/bin/time -v`, medians. Every output was identical between the two
runtimes on every run (`out/*.out`).

- protoclj: `feature/macros` 3b75e41, Release, clean worktree, on protoCore
  3.1.0 (`feature/lazy-object` 34eca79f) — `out/environment.txt`.
- bb: Babashka v1.4.192, the static release binary already in the workspace
  (`tools/bb-1.4.192/bb`).
- Inputs: `examples/showcase/gen-data.py` (seeded): access.log 115 MB,
  orders.json 43 MB (300,000 records), corpus.txt 31 MB.

| script | protoclj s | bb s | protoclj / bb | protoclj RSS MB | bb RSS MB |
|---|---:|---:|---:|---:|---:|
| start-up (empty file), wall, perf stat 3x50 | 0.024 | 0.011 | 2.3x | 23 | 17 |
| factorial 1000 with `*'` | 0.03 | 0.01 | — | 24 | 20 |
| grep over stdin (115 MB) | 18.04 | 2.08 | **8.7x** | 146 | 136 |
| JSON transform (43 MB) | 5.18 | 2.60 | 2.0x | **1967** | 339 |
| word frequencies (31 MB) | 19.17 | 4.69 | 4.1x | 159 | 144 |

The start-up row's wall time is `out/startup_wall_perf.txt` (perf stat,
three passes of 50 runs: protoclj 23.5-24.7 ms, bb 10.3-11.1 ms); the
`/usr/bin/time` rows only resolve 10 ms. `out/startup_perf_*.txt` holds a
task-clock pass whose decimals were lost to the locale (fixed in run.sh).

What the numbers say: bb is faster on every script, by 2-9x, and needs far
less memory to hold a parsed JSON document (protoCore cells are 64 bytes;
300,000 records with their strings, vectors and maps take about 2 GB of
resident memory). Where the time goes in protoclj, from one `perf` profile of
the grep on 20 MB: the `std::regex` executor (about 15 %), copying ropes to
`std::string` for each match, and allocation; no single fix stands out. The
word count spends its time the same way plus `frequencies`. Not optimised in
this work.
