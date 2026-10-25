## one primitive call per iteration (5 interleaved rounds)

ns: the loop's own timer, work minus setup (the same loop without the call).
cycles: perf stat of the whole run, work minus setup, per iteration.

| primitive | main ns | nofast ns | branch ns | main cyc | nofast cyc | branch cyc | branch vs main (cyc) | branch vs nofast (cyc) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| str | 552 | 427 | 429 | 1963 | 1517 | 1569 | -20 % | +3 % |
| assoc | 479 | 381 | 361 | 2027 | 1783 | 1783 | -12 % | +0 % |
| get | 108 | 96 | 97 | 373 | 329 | 322 | -14 % | -2 % |
| max | 134 | 105 | 117 | 418 | 370 | 391 | -6 % | +6 % |
| first | 54 | 76 | 57 | 196 | 252 | 190 | -3 % | -25 % |
| reduce-max | 94 | 96 | 120 | 319 | 335 | 418 | +31 % | +25 % |

Checksums: every variant and run computed the same result

## list-cost cases (5 interleaved rounds)

ns per element: the case's own timer (work run). cycles per element: perf stat,
work run minus setup run, every thread (the collector's included).

### n = 10000

| case | main ns | nofast ns | branch ns | main cyc | nofast cyc | branch cyc | branch vs main (cyc) | branch vs nofast (cyc) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 list walk (rest) | 211 | 313 | 207 | 545 | 1265 | 640 | +17 % | -49 % |
| 2 vector (seq v) walk | 508 | 592 | 515 | 1766 | 1809 | 2018 | +14 % | +12 % |
| 3 reduce + over a list | 60 | 74 | 51 | 141 | -260 | 164 | +16 % | n/a |
| 4 cons-build then count | 286 | 272 | 224 | 1944 | 970 | 825 | -58 % | -15 % |
| 5 sequential nth (vector) | 261 | 335 | 289 | 1232 | 1280 | 1066 | -13 % | -17 % |
| 50   control for 5 | 84 | 118 | 82 | 598 | -24 | 160 | -73 % | n/a |
| 6 random nth (vector) | 571 | 610 | 769 | 2976 | 2427 | 2720 | -9 % | +12 % |
| 60   control for 6 | 354 | 334 | 320 | 1908 | 1418 | 786 | -59 % | -45 % |
| 7 empty? + first (list) | 195 | 284 | 198 | 835 | 774 | 532 | -36 % | -31 % |
| 8 count of rest (list) | 223 | 286 | 217 | 380 | 1171 | 707 | +86 % | -40 % |
| 9 count of cons (list) | 510 | 587 | 516 | 1901 | 1858 | 1557 | -18 % | -16 % |

Peak RSS of building the list (case 1 setup run), MB: main 32, nofast 32, branch 32
Cycles of the whole setup run (start-up plus (apply list (range n))), M: main 133, nofast 134, branch 129

### n = 100000

| case | main ns | nofast ns | branch ns | main cyc | nofast cyc | branch cyc | branch vs main (cyc) | branch vs nofast (cyc) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 list walk (rest) | 246 | 342 | 246 | 991 | 1377 | 1066 | +8 % | -23 % |
| 2 vector (seq v) walk | 539 | 705 | 685 | 1742 | 2447 | 2148 | +23 % | -12 % |
| 3 reduce + over a list | 80 | 97 | 78 | 319 | 59 | 35 | -89 % | -40 % |
| 4 cons-build then count | 305 | 332 | 307 | 1132 | 1293 | 1117 | -1 % | -14 % |
| 5 sequential nth (vector) | 280 | 320 | 274 | 872 | 958 | 870 | -0 % | -9 % |
| 50   control for 5 | 73 | 100 | 69 | 367 | 521 | 62 | -83 % | -88 % |
| 6 random nth (vector) | 1416 | 1300 | 1325 | 3906 | 4513 | 4432 | +13 % | -2 % |
| 60   control for 6 | 432 | 409 | 376 | 1174 | 1416 | 1299 | +11 % | -8 % |
| 7 empty? + first (list) | 210 | 333 | 264 | 652 | 1372 | 1115 | +71 % | -19 % |
| 8 count of rest (list) | 229 | 338 | 257 | 801 | 1032 | 513 | -36 % | -50 % |
| 9 count of cons (list) | 341 | 408 | 373 | 1994 | 2638 | 2376 | +19 % | -10 % |

Peak RSS of building the list (case 1 setup run), MB: main 142, nofast 142, branch 142
Cycles of the whole setup run (start-up plus (apply list (range n))), M: main 793, nofast 689, branch 681

### n = 1000000

| case | main ns | nofast ns | branch ns | main cyc | nofast cyc | branch cyc | branch vs main (cyc) | branch vs nofast (cyc) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 list walk (rest) | 284 | 307 | 252 | 1095 | 775 | 569 | -48 % | -27 % |
| 2 vector (seq v) walk | 650 | 741 | 730 | 2904 | 4265 | 7598 | +162 % | +78 % |
| 3 reduce + over a list | 98 | 101 | 79 | 487 | 1424 | 372 | -24 % | -74 % |
| 4 cons-build then count | 479 | 506 | 473 | 2295 | 2507 | 2316 | +1 % | -8 % |
| 5 sequential nth (vector) | 301 | 358 | 314 | 969 | 1163 | 1058 | +9 % | -9 % |
| 50   control for 5 | 76 | 116 | 84 | 293 | 486 | 1872 | +538 % | +285 % |
| 6 random nth (vector) | 1937 | 2427 | 2165 | 7648 | 8904 | 9385 | +23 % | +5 % |
| 60   control for 6 | 381 | 383 | 344 | 1843 | 1388 | 6030 | +227 % | +335 % |
| 7 empty? + first (list) | 208 | 317 | 219 | 758 | 1068 | -79 | -110 % | -107 % |
| 8 count of rest (list) | 220 | 338 | 230 | 719 | 1177 | 538 | -25 % | -54 % |
| 9 count of cons (list) | 366 | 460 | 408 | 2395 | 2987 | 2559 | +7 % | -14 % |

Peak RSS of building the list (case 1 setup run), MB: main 815, nofast 773, branch 799
Cycles of the whole setup run (start-up plus (apply list (range n))), M: main 8537, nofast 8559, branch 8840

Checksums: every runtime and run computed the same result per case and n

## benchmarks/bench.sh (best of 3 per run, median of rounds, ms)

| workload | main | nofast | branch | bb | branch vs main |
|---|---:|---:|---:|---:|---:|
| fib | 409 | 438 | 435 | 452 | +6.4 % |
| tak | 41 | 40 | 42 | 26 | +2.4 % |
| sum-loop | 74 | 78 | 77 | 177 | +4.1 % |
| reduce-list | 30 | 29 | 29 | 17 | -3.3 % |
| sum-squares | 28 | 28 | 27 | 16 | -3.6 % |

Results match Babashka's in every run

## start-up, empty file (perf stat -r 50, median of rounds)

| variant | Mcycles | task-clock ms |
|---|---:|---:|
| main | 66.9 | 18.1 |
| nofast | 66.7 | 17.9 |
| branch | 65.6 | 17.6 |

## showcase scripts (wall s / peak RSS MB, median of rounds)

| script | main | nofast | branch | branch vs main (time) | outputs |
|---|---:|---:|---:|---:|---|
| startup | 0.02 / 23 | 0.02 / 23 | 0.03 / 23 | +50 % | same |
| factorial | 0.03 / 24 | 0.03 / 24 | 0.03 / 24 | +0 % | same |
| grep | 16.74 / 148 | 16.51 / 139 | 16.37 / 139 | -2 % | same |
| json | 4.92 / 1305 | 4.47 / 1138 | 4.60 / 1342 | -7 % | same |
| wordfreq | 17.48 / 158 | 19.26 / 5211 | 18.40 / 5210 | +5 % | same |

## machine state at each round (gate.txt)

    15:53:32 micro r1 idle=80% builds=0 tries=2 load=11.76 17.83 23.31
    15:55:45 micro r2 idle=81% builds=0 tries=12 load=3.54 12.66 20.67
    15:57:48 micro r3 idle=80% builds=0 tries=11 load=5.36 10.29 18.78
    16:00:40 micro r4 idle=83% builds=0 tries=16 load=3.92 8.00 16.51
    16:01:01 micro r5 idle=81% builds=0 tries=1 load=5.15 7.99 16.33
    16:02:15 cases r1 idle=83% builds=0 tries=6 load=8.74 8.79 15.98
    16:08:11 cases r2 idle=81% builds=0 tries=17 load=7.05 7.50 13.19
    16:14:40 cases r3 idle=80% builds=0 tries=20 load=4.87 6.60 10.92
    16:19:29 cases r4 idle=80% builds=0 tries=12 load=3.06 4.43 8.81
    16:42:31 cases r5 NOT-QUIET idle=60% builds=1 tries=120
    16:45:46 bench r1 idle=85% builds=0 tries=1 load=4.13 5.93 7.82
    16:46:09 bench r2 idle=82% builds=0 tries=1 load=6.08 6.23 7.86
    16:46:33 bench r3 idle=83% builds=0 tries=1 load=5.34 6.05 7.77
    16:46:56 bench r4 idle=83% builds=0 tries=1 load=7.69 6.64 7.93
    16:47:19 bench r5 idle=80% builds=0 tries=1 load=6.37 6.44 7.83
    16:47:42 startup r1 idle=83% builds=0 tries=1 load=5.71 6.28 7.74
    16:47:55 startup r2 idle=82% builds=0 tries=1 load=5.03 6.10 7.66
    16:48:09 startup r3 idle=80% builds=0 tries=1 load=6.28 6.33 7.71
    16:48:22 startup r4 idle=81% builds=0 tries=1 load=5.54 6.17 7.64
    16:48:36 startup r5 idle=83% builds=0 tries=1 load=4.73 5.96 7.55
    16:49:23 showcase r1 idle=80% builds=0 tries=3 load=8.04 6.58 7.68
    16:51:30 showcase r2 idle=84% builds=0 tries=1 load=5.15 6.16 7.39
    16:53:38 showcase r3 idle=83% builds=0 tries=1 load=3.40 5.43 6.98
    16:55:46 showcase r4 idle=84% builds=0 tries=1 load=10.02 7.18 7.41
    16:59:47 showcase r5 idle=80% builds=0 tries=11 load=4.88 6.04 6.91
    done 17:02:02
