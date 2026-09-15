;; EXPECT: :x :x true :y 1
;; A map used as a map key is hashed and matched by value: an equal map
;; built separately, in any insertion order, finds, replaces the entry.
(def m {{:a 1 :b 2} :x})
(def replaced (assoc m {:b 2 :a 1} :y))
(println (get m {:b 2 :a 1})
         (get m (hash-map :a 1 :b 2))
         (contains? m {:a 1 :b 2})
         (get replaced {:a 1 :b 2})
         (count (keys replaced)))
