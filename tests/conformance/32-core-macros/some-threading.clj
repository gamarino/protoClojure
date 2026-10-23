;; EXPECT: 3 nil nil 6
;; some-> and some->> stop at the first nil.
(println (some-> {:a 1} :a inc inc) (some-> {:a 1} :b inc) (some->> nil (map inc))
         (some->> [1 2 3] (reduce +)))
