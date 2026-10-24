;; EXPECT: (0 2) (0 2) (1 2 3) (1 2 0 1 2)
;; conj and cons put cons cells in front of a lazy seq without realising it;
;; list* too.
(println (conj (map inc [1]) 0) (cons 0 (map inc [1])) (list* 1 2 (map inc [2]))
         (take 5 (list* 1 2 (range))))
