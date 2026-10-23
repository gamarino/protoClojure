;; EXPECT: (0 1 2 3 4) (2 3 4) (10 7 4 1) () (0 0.5 1.0 1.5)
;; range with an end, a start and a step (eager and finite until lazy
;; sequences exist).
(println (range 5) (range 2 5) (range 10 0 -3) (range 0) (range 0 2 0.5))
