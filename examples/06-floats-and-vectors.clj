;; Floats: arithmetic promotes int → float.
(println (+ 1 2.5))
(println (/ 1.0 7.0))

;; Vectors: a distinct sequential type.
(println (vec (list 1 2 3 4 5)))
(println (nth [:a :b :c :d] 2))
(println (vector? [1 2 3]))

;; Higher-order over vectors.
(println (reduce + [1 2 3 4 5]))
(println (map (fn [x] (* x x)) [1.0 2.0 3.0 4.0 5.0]))
