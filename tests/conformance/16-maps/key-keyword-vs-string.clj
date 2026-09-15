;; EXPECT: nil nil false 1 2 2 false true
;; A keyword and a string with the same spelling are different map keys:
;; neither finds the other, and both can be keys of one map.
(def m (hash-map :a 1 ":a" 2))
(println (get {:a 1} ":a")
         (get {":a" 1} :a)
         (contains? {:a 1} ":a")
         (get m :a)
         (get m ":a")
         (count (keys m))
         (string? (first (keys m)))
         (string? (first (rest (keys m)))))
