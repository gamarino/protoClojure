;; EXPECT: 15 106
(println (reduce + (list 1 2 3 4 5))
         (reduce + 100 (list 1 2 3)))
