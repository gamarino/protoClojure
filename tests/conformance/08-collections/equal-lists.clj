;; EXPECT: true false false false true
;; Two lists built separately are = when they hold equal elements in the
;; same order.
(println (= (list 1 2) (list 1 2))
         (= (list 1 2) (list 1 3))
         (= (list 1 2) (list 1 2 3))
         (= (list 1 2) (list 2 1))
         (not= (list 1 2) (list 2 1)))
