(def v (vec (range 300000)))
(println (reduce + (map inc v)) (count (filter even? v)))
