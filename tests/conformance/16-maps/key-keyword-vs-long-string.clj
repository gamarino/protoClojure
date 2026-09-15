;; EXPECT: nil nil 2 1 2
;; A keyword and a string with the same long spelling are different keys.
(def m (hash-map :a-long-keyword 1 ":a-long-keyword" 2))
(println (get {:a-long-keyword 1} ":a-long-keyword")
         (get {":a-long-keyword" 1} :a-long-keyword)
         (count m)
         (get m :a-long-keyword)
         (get m (str ":a-long-" "keyword")))
