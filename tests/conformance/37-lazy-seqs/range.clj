;; EXPECT: (0 1 2 3 4) (1 2 3 4) (0 3 6 9) (5 3 1) (0 0.25 0.5 0.75) () 100000 (7 7 7)
;; range: finite and lazy, with a step, negative steps and floats; a zero
;; step repeats start unless start equals end.
(println (range 5) (range 1 5) (range 0 10 3) (range 5 0 -2) (range 0 1 0.25)
         (range 3 3) (count (range 100000)) (take 3 (range 7 9 0)))
