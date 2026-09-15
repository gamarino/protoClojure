;; EXPECT: {:a 1, :b 2} {:a 1, :c 3} {:a 1}
;; Maps are persistent: two assocs on the same map produce independent maps
;; and leave the original unchanged.
(def m {:a 1})
(println (assoc m :b 2) (assoc m :c 3) m)
