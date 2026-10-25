## one primitive call per iteration (5 interleaved rounds)

ns: the loop's own timer, work minus setup (the same loop without the call).
cycles: perf stat of the whole run, work minus setup, per iteration.

| primitive | main ns | nofast ns | branch ns | main cyc | nofast cyc | branch cyc | branch vs main (cyc) | branch vs nofast (cyc) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| str | 562 | 411 | 429 | 1935 | 1449 | 1489 | -23 % | +3 % |
| assoc | 484 | 407 | 420 | 2016 | 1721 | 1772 | -12 % | +3 % |
| get | 116 | 111 | 100 | 391 | 350 | 349 | -11 % | -0 % |
| max | 122 | 97 | 112 | 421 | 356 | 371 | -12 % | +4 % |
| first | 58 | 71 | 55 | 186 | 250 | 185 | -1 % | -26 % |
| reduce-max | 112 | 101 | 96 | 355 | 332 | 337 | -5 % | +1 % |

Checksums: every variant and run computed the same result

## list-cost cases (5 interleaved rounds)

ns per element: the case's own timer (work run). cycles per element: perf stat,
work run minus setup run, every thread (the collector's included).

### n = 10000

| case | main ns | nofast ns | branch ns | main cyc | nofast cyc | branch cyc | branch vs main (cyc) | branch vs nofast (cyc) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 list walk (rest) | 202 | 254 | 209 | 1246 | 557 | 1253 | +1 % | +125 % |
| 2 vector (seq v) walk | 477 | 516 | 452 | 2226 | 1892 | 1258 | -43 % | -34 % |
| 3 reduce + over a list | 46 | 66 | 46 | -345 | -390 | 156 | n/a | n/a |
| 4 cons-build then count | 208 | 261 | 208 | 478 | 960 | 492 | +3 % | -49 % |
| 5 sequential nth (vector) | 252 | 309 | 283 | 996 | 871 | 1048 | +5 % | +20 % |
| 50   control for 5 | 76 | 111 | 86 | 405 | 361 | 426 | +5 % | +18 % |
| 6 random nth (vector) | 570 | 619 | 535 | 2505 | 1599 | 1586 | -37 % | -1 % |
| 60   control for 6 | 321 | 336 | 300 | 1023 | 1195 | 1021 | -0 % | -15 % |
| 7 empty? + first (list) | 186 | 263 | 194 | 628 | 980 | 1044 | +66 % | +6 % |
| 8 count of rest (list) | 207 | 284 | 208 | 921 | 1121 | 609 | -34 % | -46 % |
| 9 count of cons (list) | 547 | 583 | 502 | 2129 | 1664 | 2243 | +5 % | +35 % |

Peak RSS of building the list (case 1 setup run), MB: main 32, nofast 32, branch 32
Cycles of the whole setup run (start-up plus (apply list (range n))), M: main 129, nofast 132, branch 129

### n = 100000

| case | main ns | nofast ns | branch ns | main cyc | nofast cyc | branch cyc | branch vs main (cyc) | branch vs nofast (cyc) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 list walk (rest) | 241 | 304 | 257 | 923 | 1010 | 1235 | +34 % | +22 % |
| 2 vector (seq v) walk | 451 | 524 | 498 | 1667 | 1765 | 1767 | +6 % | +0 % |
| 3 reduce + over a list | 70 | 94 | 71 | 150 | 503 | 196 | +30 % | -61 % |
| 4 cons-build then count | 287 | 340 | 269 | 1166 | 1355 | 1072 | -8 % | -21 % |
| 5 sequential nth (vector) | 259 | 320 | 268 | 998 | 1180 | 996 | -0 % | -16 % |
| 50   control for 5 | 66 | 97 | 66 | 179 | 165 | 244 | +36 % | +48 % |
| 6 random nth (vector) | 1046 | 1069 | 1100 | 4075 | 4117 | 4412 | +8 % | +7 % |
| 60   control for 6 | 330 | 330 | 303 | 979 | 1094 | 1012 | +3 % | -8 % |
| 7 empty? + first (list) | 178 | 259 | 186 | 634 | 748 | 832 | +31 % | +11 % |
| 8 count of rest (list) | 199 | 297 | 204 | 586 | 1026 | 766 | +31 % | -25 % |
| 9 count of cons (list) | 317 | 397 | 314 | 2770 | 2511 | 2269 | -18 % | -10 % |

Peak RSS of building the list (case 1 setup run), MB: main 142, nofast 142, branch 142
Cycles of the whole setup run (start-up plus (apply list (range n))), M: main 812, nofast 696, branch 680

### n = 1000000

| case | main ns | nofast ns | branch ns | main cyc | nofast cyc | branch cyc | branch vs main (cyc) | branch vs nofast (cyc) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 list walk (rest) | 248 | 298 | 251 | 906 | 757 | 1816 | +100 % | +140 % |
| 2 vector (seq v) walk | 563 | 644 | 560 | 3001 | 3203 | 2777 | -7 % | -13 % |
| 3 reduce + over a list | 82 | 95 | 42 | 275 | 63 | 945 | +243 % | +1410 % |
| 4 cons-build then count | 410 | 473 | 417 | 2282 | 2455 | 2282 | -0 % | -7 % |
| 5 sequential nth (vector) | 265 | 338 | 271 | 928 | 1163 | 935 | +1 % | -20 % |
| 50   control for 5 | 73 | 102 | 74 | 288 | -96 | -167 | -158 % | n/a |
| 6 random nth (vector) | 1977 | 1930 | 1883 | 7346 | 7618 | 7063 | -4 % | -7 % |
| 60   control for 6 | 337 | 339 | 320 | 1150 | 1108 | 996 | -13 % | -10 % |
| 7 empty? + first (list) | 197 | 286 | 206 | 497 | 1591 | 1539 | +209 % | -3 % |
| 8 count of rest (list) | 212 | 307 | 209 | 639 | 875 | 1672 | +162 % | +91 % |
| 9 count of cons (list) | 337 | 451 | 402 | 2378 | 2144 | 2234 | -6 % | +4 % |

Peak RSS of building the list (case 1 setup run), MB: main 811, nofast 819, branch 788
Cycles of the whole setup run (start-up plus (apply list (range n))), M: main 8551, nofast 8746, branch 7850

Checksums: every runtime and run computed the same result per case and n

## benchmarks/bench.sh (best of 3 per run, median of rounds, ms)

| workload | main | nofast | branch | bb | branch vs main |
|---|---:|---:|---:|---:|---:|
| fib | 380 | 384 | 393 | 425 | +3.4 % |
| tak | 40 | 38 | 40 | 26 | +0.0 % |
| sum-loop | 72 | 73 | 75 | 167 | +4.2 % |
| reduce-list | 29 | 29 | 30 | 17 | +3.4 % |
| sum-squares | 28 | 27 | 28 | 15 | +0.0 % |

Results match Babashka's in every run

## start-up, empty file (perf stat -r 50, median of rounds)

| variant | Mcycles | task-clock ms |
|---|---:|---:|
| main | 67.7 | 18.4 |
| nofast | 66.4 | 17.7 |
| branch | 66.1 | 18.2 |

## showcase scripts (wall s / peak RSS MB, median of rounds)

| script | main | nofast | branch | branch vs main (time) | outputs |
|---|---:|---:|---:|---:|---|
| startup | 0.02 / 23 | 0.04 / 23 | 0.02 / 23 | +0 % | same |
| factorial | 0.03 / 24 | 0.03 / 23 | 0.03 / 23 | +0 % | same |
| grep | 16.27 / 148 | 15.87 / 139 | 16.37 / 139 | +1 % | same |
| json | 4.36 / 1515 | 4.21 / 1134 | 4.14 / 1248 | -5 % | same |
| wordfreq | 17.09 / 159 | 16.14 / 144 | 16.27 / 144 | -5 % | same |

## machine state at each round (gate.txt)

    17:23:58 micro r1 idle=81% builds=0 tries=1 load=5.07 6.59 6.56
    17:24:20 micro r2 idle=80% builds=0 tries=1 load=6.21 6.71 6.60
    17:24:42 micro r3 idle=80% builds=0 tries=1 load=5.02 6.40 6.50
    17:25:03 micro r4 idle=82% builds=0 tries=1 load=4.71 6.23 6.44
    17:25:25 micro r5 idle=81% builds=0 tries=1 load=5.81 6.41 6.50
    17:26:06 cases r1 idle=83% builds=0 tries=3 load=4.11 5.89 6.32
    17:28:20 cases r2 idle=82% builds=0 tries=1 load=9.85 7.87 7.03
    17:30:35 cases r3 idle=83% builds=0 tries=1 load=5.68 7.56 7.06
    17:32:47 cases r4 idle=82% builds=0 tries=1 load=7.81 7.03 6.89
    17:35:00 cases r5 idle=83% builds=0 tries=1 load=9.90 7.84 7.19
    17:37:12 bench r1 idle=83% builds=0 tries=1 load=6.92 7.80 7.28
    17:37:35 bench r2 idle=84% builds=0 tries=1 load=5.13 7.31 7.14
    17:37:57 bench r3 idle=83% builds=0 tries=1 load=3.87 6.89 7.00
    17:38:19 bench r4 idle=82% builds=0 tries=1 load=4.55 6.84 6.98
    17:38:42 bench r5 idle=83% builds=0 tries=1 load=3.45 6.44 6.85
    17:39:04 startup r1 idle=84% builds=0 tries=1 load=2.88 6.07 6.71
    17:39:18 startup r2 idle=83% builds=0 tries=1 load=2.59 5.90 6.65
    17:39:32 startup r3 idle=83% builds=0 tries=1 load=4.66 6.20 6.74
    17:39:45 startup r4 idle=82% builds=0 tries=1 load=3.99 5.97 6.65
    17:39:59 startup r5 idle=83% builds=0 tries=1 load=3.69 5.81 6.59
    17:40:32 showcase r1 idle=83% builds=0 tries=3 load=5.32 6.13 6.68
    17:42:33 showcase r2 idle=83% builds=0 tries=1 load=2.95 5.08 6.23
    17:44:34 showcase r3 idle=84% builds=0 tries=1 load=4.27 4.80 5.97
    17:46:35 showcase r4 idle=84% builds=0 tries=1 load=4.40 4.81 5.84
    17:48:37 showcase r5 idle=84% builds=0 tries=1 load=3.48 4.36 5.54
    done 17:50:29
