;; EXPECT: 3 true 6
;; keys and vals of one map walk its entries in the same order, so each value
;; is the one under the matching key. The order itself is unspecified.
(def m {:z 1 :y 2 :x 3})
(println (count (keys m))
         (= (map m (keys m)) (vals m))
         (reduce + (vals m)))
