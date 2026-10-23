;; EXPECT: true true true true true
;; hash-set, set over a vector, a list, a string and nil; duplicates collapse.
(println (= (hash-set :a :b :a) #{:a :b})
         (= (set [1 2 2 3]) #{1 2 3})
         (= (set '(1 1)) #{1})
         (= (set "aba") #{"a" "b"})
         (= (set nil) #{}))
