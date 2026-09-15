;; EXPECT: true 3.5 18014398509481986 true false
;; Numbers of every kind still mix in arithmetic and comparisons, and the
;; one-argument comparison is true for any value, as in Clojure.
(println (< "a") (+ 1 2.5) (* 2 9007199254740993) (< 1 2.5) (>= 1.5 9007199254740993))
