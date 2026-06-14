;; EXPECT: 10 (20 30) (0 1 2 3)
(println (first (list 10 20 30))
         (rest  (list 10 20 30))
         (cons 0 (list 1 2 3)))
