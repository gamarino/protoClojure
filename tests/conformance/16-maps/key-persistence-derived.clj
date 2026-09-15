;; EXPECT: :a :b 0 1 :a
;; Maps derived from one map with collection keys are independent, and the
;; original is unchanged.
(def m {[1 2] :a})
(def m2 (assoc m (list 1 2) :b))
(def m3 (dissoc m [1 2]))
(println (get m [1 2]) (get m2 [1 2]) (count m3) (count m) (get m (list 1 2)))
