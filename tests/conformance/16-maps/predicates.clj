;; EXPECT: true false false
(println (map? {:a 1})
         (map? [1 2])
         (map? (list 1 2)))
