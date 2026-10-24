(def v (vec (range 300000)))
(println (loop [s (seq v) acc 0] (if s (recur (next s) (+ acc (first s))) acc)))
