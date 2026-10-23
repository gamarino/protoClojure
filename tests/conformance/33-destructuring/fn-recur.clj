;; EXPECT: 6
;; recur in a fn with a destructured parameter rebinds the parameter, which
;; the body destructures again.
(defn total [[x & more] acc]
  (if x (recur more (+ acc x)) acc))
(println (total [1 2 3] 0))
