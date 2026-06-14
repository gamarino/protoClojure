;; EXPECT: :b
(println (cond (< 5 2) :a (> 5 2) :b :else :else))
