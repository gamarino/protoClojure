;; EXPECT: 0 (0 1 2 3 4) 100
;; (range) with no end is a lazy seq without end: take what you need.
(println (first (range)) (take 5 (range)) (nth (range) 100))
