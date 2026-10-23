;; EXPECT: (1 2 3) (1 2 3) (0 1 2 3) 1 (2 3) [1 2 3]
;; docs/tutorial/04-data-structures.md §4.6 and §4.3 (vec of a quoted list).
(println '(1 2 3) (list 1 2 3) (cons 0 '(1 2 3)) (first '(1 2 3)) (rest '(1 2 3)) (vec '(1 2 3)))
