;; EXPECT: (:b 1 :a 2)
;; Trailing keyword/value arguments to an ordinary callee keep their source
;; order (they used to be packed into a map and come back in hash order).
(println (list :b 1 :a 2))
