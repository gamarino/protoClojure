;; EXPECT: nil
(println (cond (< 5 2) :a (< 10 2) :b))
