;; EXPECT: true true true false
;; Elements are compared with = recursively, so nested lists and vectors
;; (also inside maps) compare by value whatever their concrete type.
(println (= [1 [2]] (list 1 (list 2)))
         (= (list [1 (list 2 3)]) [(list 1 [2 3])])
         (= {:a (list 1 [2 3])} {:a [1 (list 2 3)]})
         (= [1 [2]] (list 1 (list 3))))
