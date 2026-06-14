;; EXPECT: 5 true false
(println (count (list 1 2 3 4 5))
         (empty? (list))
         (empty? (list 1)))
