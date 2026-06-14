;; EXPECT: true false
(println (contains? {:a 1 :b 2} :a)
         (contains? {:a 1 :b 2} :z))
