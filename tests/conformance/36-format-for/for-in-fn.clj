;; EXPECT: [0 2 4] 12
;; for inside a function, over a local, with the result used as a seq.
(defn evens [n] (vec (for [i (range n) :when (even? i)] i)))
(defn total [xs] (reduce + (for [x xs y [1 2]] (* x y))))
(println (evens 6) (total [1 3]))
