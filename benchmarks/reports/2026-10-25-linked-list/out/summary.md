## list-cost cases (5 interleaved rounds)

ns per element: the case's own timer (work run). cycles per element: perf stat,
work run minus setup run, every thread (the collector's included).

### n = 10000

| case | main ns | A ns | B ns | bb ns | main cyc | A cyc | B cyc | B vs main (cyc) | B vs A (cyc) | cells/el main / B |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 list walk (rest) | 216 | 215 | 395 | 189 | -68 | 984 | 1716 | n/a | +74 % | 0.0 / 0.0 |
| 2 vector (seq v) walk | 491 | 501 | 751 | 210 | 1995 | 1524 | 2274 | +14 % | +49 % | 0.0 / 0.0 |
| 3 reduce + over a list | 51 | 52 | 38 | 72 | 455 | 294 | 24 | -95 % | -92 % | 0.0 / 0.0 |
| 4 cons-build then count | 245 | 236 | 270 | 190 | 770 | 440 | 739 | -4 % | +68 % | 0.0 / 0.0 |
| 5 sequential nth (vector) | 280 | 267 | 255 | 222 | 1541 | 924 | 951 | -38 % | +3 % | 0.0 / 0.0 |
| 50   control for 5 | 98 | 98 | 96 | 180 | -113 | 458 | 398 | n/a | -13 % | 0.0 / 0.0 |
| 6 random nth (vector) | 618 | 655 | 558 | 477 | 2186 | 2364 | 1795 | -18 % | -24 % | 0.0 / 0.0 |
| 60   control for 6 | 349 | 318 | 318 | 460 | 1105 | 1512 | 912 | -17 % | -40 % | 0.0 / 0.0 |
| 7 empty? + first (list) | 202 | 209 | 169 | 267 | 695 | 542 | 481 | -31 % | -11 % | 0.0 / 0.0 |
| 8 count of rest (list) | 218 | 246 | 447 | 226 | 1243 | 888 | 1715 | +38 % | +93 % | 0.0 / 0.0 |
| 9 count of cons (list) | 637 | 535 | 515 | 318 | 2241 | 1643 | 2005 | -11 % | +22 % | 0.0 / 0.0 |
| 10 (vec list) | 1090 | 1039 | 1036 | 79 | 3450 | 3426 | 3370 | -2 % | -2 % | 0.0 / 0.0 |

Peak RSS of building the list (case 1 setup run), MB: main 32, A 32, B 28
Cycles of the whole setup run (start-up plus (apply list (range n))), M: main 140, A 136, B 119

### n = 100000

| case | main ns | A ns | B ns | bb ns | main cyc | A cyc | B cyc | B vs main (cyc) | B vs A (cyc) | cells/el main / B |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 list walk (rest) | 247 | 250 | 179 | 181 | 794 | 844 | 636 | -20 % | -25 % | 0.0 / 0.0 |
| 2 vector (seq v) walk | 491 | 488 | 497 | 190 | 1649 | 1606 | 1684 | +2 % | +5 % | 1.9 / 1.9 |
| 3 reduce + over a list | 78 | 73 | 19 | 36 | 282 | 12 | 77 | -73 % | +522 % | -0.0 / 0.0 |
| 4 cons-build then count | 332 | 296 | 310 | 162 | 1227 | 1195 | 1182 | -4 % | -1 % | 0.0 / 0.0 |
| 5 sequential nth (vector) | 275 | 285 | 270 | 198 | 832 | 828 | 869 | +4 % | +5 % | 1.9 / 1.9 |
| 50   control for 5 | 85 | 81 | 76 | 147 | 377 | 310 | 181 | -52 % | -42 % | 1.9 / 1.9 |
| 6 random nth (vector) | 1087 | 1061 | 1053 | 499 | 3912 | 3793 | 3687 | -6 % | -3 % | 1.9 / 1.9 |
| 60   control for 6 | 320 | 312 | 320 | 401 | 1196 | 1031 | 971 | -19 % | -6 % | 1.9 / 1.9 |
| 7 empty? + first (list) | 203 | 208 | 176 | 194 | 521 | 753 | 531 | +2 % | -30 % | 0.0 / 0.0 |
| 8 count of rest (list) | 224 | 212 | 195 | 184 | 832 | 553 | 990 | +19 % | +79 % | 0.0 / 0.0 |
| 9 count of cons (list) | 294 | 345 | 297 | 247 | 2173 | 2115 | 1253 | -42 % | -41 % | 18.3 / 0.0 |
| 10 (vec list) | 515 | 572 | 389 | 24 | 4153 | 4231 | 2590 | -38 % | -39 % | 30.5 / 30.0 |

Peak RSS of building the list (case 1 setup run), MB: main 142, A 142, B 141
Cycles of the whole setup run (start-up plus (apply list (range n))), M: main 692, A 693, B 668

### n = 1000000

| case | main ns | A ns | B ns | bb ns | main cyc | A cyc | B cyc | B vs main (cyc) | B vs A (cyc) | cells/el main / B |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 list walk (rest) | 245 | 268 | 217 | 169 | 1107 | 130 | 793 | -28 % | +508 % | 1.1 / 0.2 |
| 2 vector (seq v) walk | 625 | 652 | 677 | 272 | 2915 | 2740 | 3016 | +3 % | +10 % | 9.3 / 4.3 |
| 3 reduce + over a list | 89 | 50 | 21 | 27 | 202 | 889 | -57 | -128 % | -106 % | 1.0 / 0.0 |
| 4 cons-build then count | 474 | 461 | 462 | 352 | 2274 | 2325 | 1733 | -24 % | -25 % | 2.0 / 2.0 |
| 5 sequential nth (vector) | 318 | 307 | 302 | 274 | 1077 | 1245 | 1237 | +15 % | -1 % | 0.6 / 0.5 |
| 50   control for 5 | 84 | 76 | 89 | 227 | 124 | -234 | 231 | +85 % | n/a | -0.1 / -0.2 |
| 6 random nth (vector) | 1964 | 2145 | 2050 | 757 | 7081 | 7315 | 7091 | +0 % | -3 % | -0.1 / -0.3 |
| 60   control for 6 | 354 | 395 | 338 | 464 | 1144 | 1033 | 1246 | +9 % | +21 % | -0.0 / 0.5 |
| 7 empty? + first (list) | 225 | 216 | 177 | 183 | 833 | 1494 | 381 | -54 % | -74 % | -0.4 / -0.2 |
| 8 count of rest (list) | 238 | 239 | 239 | 168 | 186 | 710 | 1094 | +489 % | +54 % | -3.7 / 0.6 |
| 9 count of cons (list) | 439 | 382 | 428 | 242 | 2112 | 2186 | 2306 | +9 % | +5 % | 10.7 / 8.4 |
| 10 (vec list) | 1094 | 1130 | 768 | 18 | 6145 | 6219 | 4785 | -22 % | -23 % | 17.9 / 21.0 |

Peak RSS of building the list (case 1 setup run), MB: main 788, A 789, B 562
Cycles of the whole setup run (start-up plus (apply list (range n))), M: main 8340, A 8387, B 7335

Checksums: every runtime and run computed the same result per case and n

## benchmarks/bench.sh (best of 3 per run, median of rounds, ms)

| workload | main | A | B | bb | B vs main |
|---|---:|---:|---:|---:|---:|
| fib | 421 | 421 | 422 | 481 | +0.2 % |
| tak | 44 | 42 | 40 | 31 | -9.1 % |
| sum-loop | 78 | 78 | 80 | 176 | +2.6 % |
| reduce-list | 32 | 32 | 31 | 21 | -3.1 % |
| sum-squares | 31 | 32 | 30 | 19 | -3.2 % |

Results match Babashka's in every run

## start-up, empty file (perf stat -r 50, median of rounds)

| variant | Mcycles | task-clock ms |
|---|---:|---:|
| main | 67.2 | 20.5 |
| A | 66.9 | 19.6 |
| B | 65.0 | 19.2 |

## showcase scripts (wall s / peak RSS MB, median of rounds)

| script | main | A | B | bb | B vs main (time) | outputs |
|---|---:|---:|---:|---:|---:|---|
| startup | 0.03 / 23 | 0.03 / 23 | 0.03 / 23 | 0.00 / 15 | +0 % | same |
| factorial | 0.04 / 23 | 0.03 / 23 | 0.04 / 24 | 0.01 / 18 | +0 % | same |
| grep | 18.26 / 139 | 17.78 / 139 | 17.84 / 139 | 2.06 / 134 | -2 % | same |
| json | 4.60 / 1371 | 4.68 / 1375 | 4.54 / 1223 | 2.62 / 337 | -1 % | same |
| wordfreq | 18.14 / 144 | 18.05 / 144 | 18.01 / 144 | 4.80 / 147 | -1 % | same |

## machine state at each round (gate.txt)

    00:09:09 cases r1 idle=76% builds=0 load=18.82 10.36 8.09
    00:11:55 cases r2 idle=78% builds=0 load=7.50 8.68 7.80
    00:14:43 cases r3 idle=74% builds=0 load=5.08 7.16 7.35
    00:17:29 cases r4 idle=76% builds=0 load=8.36 8.13 7.70
    00:20:13 cases r5 idle=76% builds=0 load=5.98 7.53 7.58
    00:23:08 bench r1 idle=74% builds=0 load=8.50 8.64 8.02
    00:23:33 bench r2 idle=79% builds=0 load=8.82 8.68 8.04
    00:23:57 bench r3 idle=75% builds=0 load=10.56 9.03 8.17
    00:24:21 bench r4 idle=73% builds=0 load=11.55 9.38 8.31
    00:24:45 bench r5 idle=75% builds=0 load=9.80 9.18 8.27
    00:25:08 startup r1 idle=76% builds=0 load=8.22 8.85 8.18
    00:25:22 startup r2 idle=72% builds=0 load=8.39 8.85 8.19
    00:25:36 startup r3 idle=76% builds=0 load=6.74 8.47 8.08
    00:25:50 startup r4 idle=78% builds=0 load=9.00 8.94 8.24
    00:26:04 startup r5 idle=47% builds=0 load=12.09 9.63 8.48
    00:26:18 showcase r1 idle=76% builds=0 load=10.78 9.42 8.42
    00:28:41 showcase r2 idle=78% builds=0 load=7.98 8.09 8.02
    00:31:03 showcase r3 idle=57% builds=0 load=6.60 7.27 7.71
    00:33:38 showcase r4 idle=72% builds=0 load=10.28 8.86 8.26
    00:35:59 showcase r5 idle=77% builds=0 load=7.58 8.10 8.05
    done 00:38:17
