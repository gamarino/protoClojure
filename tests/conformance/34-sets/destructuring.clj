;; EXPECT: true 3
;; A set destructures sequentially through seq (its order is unspecified).
(let [[a & more] #{1 2 3}] (println (contains? #{1 2 3} a) (inc (count more))))
