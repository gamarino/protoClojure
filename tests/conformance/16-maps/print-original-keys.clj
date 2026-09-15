;; EXPECT: {(1 2) :a} {"a-long-string-key" 1} {1.5 :d} ((1 2)) true {{:a 1} :m} {-0.0 :z} (100000000000000000000)
;; A map prints, and keys returns, the key objects the entries were created
;; with: a list key stays a list, a string stays a string, a double and a big
;; integer print as numbers.
(println {(list 1 2) :a}
         (str {"a-long-string-key" 1})
         {1.5 :d}
         (keys {(list 1 2) :a})
         (list? (first (keys {(list 1 2) :a})))
         {{:a 1} :m}
         {-0.0 :z}
         (keys {100000000000000000000 :big}))
