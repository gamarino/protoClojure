## bench.sh workloads (ms, best of 3 per round; median [min-max] of rounds)

| workload | protoclj | bb | ratio (medians) | protoclj / bb per round |
|---|---:|---:|---:|---|
| fib | 421 [402-454] | 455 [423-469] | 0.93x | 0.97, 0.99, 0.93, 0.90, 0.95 |
| tak | 41 [38-46] | 29 [28-45] | 1.41x | 1.60, 1.00, 1.38, 1.35, 1.33 |
| sum-loop | 77 [73-81] | 172 [165-198] | 0.45x | 0.41, 0.44, 0.43, 0.47, 0.44 |
| reduce-list | 29 [28-33] | 19 [19-20] | 1.54x | 1.59, 1.55, 1.55, 1.47, 1.50 |
| sum-squares | 28 [27-34] | 17 [17-18] | 1.61x | 1.62, 1.96, 1.61, 1.59, 1.64 |

Runs checked: 150, failed: 0.

```
protoclj factorial-100: rc=0 digits=158
bb factorial-100: rc=1 first line: Message:  long overflow
```

## Start-up (3 passes x 50 runs, interleaved)

```
hello  protoclj  wall median   20.9 ms (per-pass medians 21.2, 20.7, 20.9; p10-p90 19.7-24.1) | peak RSS median  24.2 MB (min 23.8, max 24.7) | runs 150
hello  bb        wall median    9.7 ms (per-pass medians 9.7, 9.6, 9.8; p10-p90 9.0-10.9) | peak RSS median  15.4 MB (min 15.2, max 15.7) | runs 150
empty  protoclj  wall median   20.9 ms (per-pass medians 21.7, 21.1, 20.5; p10-p90 19.4-25.4) | peak RSS median  24.2 MB (min 23.9, max 24.6) | runs 150
empty  bb        wall median    9.7 ms (per-pass medians 9.9, 9.7, 9.5; p10-p90 8.8-11.4) | peak RSS median  15.4 MB (min 15.2, max 15.6) | runs 150
failures: 0
```

## Showcase (run.sh of 2026-10-23-showcase)

```
workload    protoclj s     bb s  ratio  protoclj MB   bb MB  output
startup           0.02     0.00    inf           24      15  same: (empty)
factorial         0.02     0.01    2.0           24      18  same: digits 2568 digit-sum 10539 head 402387260077093773543702433
grep             16.69     1.93    8.6          140     134  same: matches 1588 latency-ms-total 2053520
json              4.13     2.51    1.6         1424     338  same: orders 300000 kept 106469
wordfreq         17.20     4.67    3.7          143     143  same: 381941 ribagori
start-up protoclj: task-clock 18.4 ms (median of 3 passes x 50 runs: 18.4, 18.5, 17.3)
start-up bb: task-clock 7.4 ms (median of 3 passes x 50 runs: 6.9, 7.6, 7.4)
all outputs identical across runtimes and runs
```

## Actors (msg/s; median [min-max] over rounds)

| mode | w=1 | w=2 | w=4 | w=6 | w=8 | w=16 | peak (median) |
|---|---:|---:|---:|---:|---:|---:|---:|
| single | 136,743 [128,436-215,332] | 169,866 [167,842-216,685] | 174,703 [165,453-177,968] | 160,026 [143,513-162,999] | 195,925 [179,727-239,120] | 207,082 [202,675-211,104] | 207,082 (w=16) |
| fan-out | 320,718 [257,268-333,778] | 448,632 [334,001-482,625] | 382,995 [357,654-417,014] | 406,339 [380,807-411,523] | 394,322 [377,501-421,941] | 399,840 [398,724-403,388] | 448,632 (w=2) |
| MPSC | 180,115 [166,889-192,271] | 185,563 [164,935-213,950] | 210,704 [190,658-217,439] | 178,349 [169,981-182,582] | 167,757 [162,311-181,752] | 160,798 [154,083-197,785] | 210,704 (w=4) |
| MPMC | 220,411 [174,005-231,858] | 236,017 [210,927-263,021] | 280,113 [272,852-290,614] | 251,890 [241,547-306,938] | 211,194 [194,629-249,814] | 170,970 [170,475-188,325] | 280,113 (w=4) |

Failed actor runs: 0.

## Load

```
bench-r1 01:30:28 load 7.43 6.59 6.86 waited 15s
bench-r2 01:30:35 load 6.59 6.44 6.81 waited 0s
bench-r3 01:30:43 load 6.22 6.36 6.78 waited 0s
bench-r4 01:30:50 load 5.63 6.23 6.73 waited 0s
bench-r5 01:30:57 load 5.34 6.16 6.71 waited 0s
bench end 01:31:01 load 4.99 6.08 6.68 status 0
startup-p1 01:31:03 load 4.99 6.08 6.68 waited 0s
startup-p2 01:31:09 load 4.83 6.03 6.66 waited 0s
startup-p3 01:31:15 load 4.16 5.84 6.59 waited 0s
startup end 01:31:18 load 4.16 5.84 6.59 status 0
showcase-round 01:31:21 load 4.15 5.81 6.58 waited 0s
showcase-round 01:32:09 load 3.62 5.45 6.42 waited 0s
showcase-round 01:32:59 load 4.94 5.52 6.39 waited 0s
showcase-round 01:33:50 load 4.49 5.35 6.28 waited 0s
showcase-round 01:34:39 load 3.77 5.02 6.13 waited 0s
showcase end 01:35:35 load 5.76 5.41 6.19 status 0
actors-r1 01:35:37 load 5.76 5.41 6.19 waited 0s
actors-r1 end 01:37:30 load 10.75 6.85 6.59 status 0
actors-r2 01:37:32 load 10.75 6.85 6.59 waited 0s
actors-r2 end 01:39:19 load 5.77 6.27 6.41 status 0
actors-r3 01:39:21 load 5.39 6.18 6.38 waited 0s
actors-r3 end 01:41:14 load 9.78 7.52 6.83 status 0
```
