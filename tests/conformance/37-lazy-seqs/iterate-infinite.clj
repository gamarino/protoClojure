;; EXPECT: (0 1 2 3 4) 1024
;; iterate is endless; take reads only what it needs.
(println (take 5 (iterate inc 0)) (nth (iterate (fn [x] (* 2 x)) 1) 10))
