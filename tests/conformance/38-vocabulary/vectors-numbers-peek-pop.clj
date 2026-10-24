;; EXPECT: [[2 3 4] [11 22] [1 3 5] 20 3 [1 2] 1 (2) nil nil true false true true false true true true]
;; mapv, filterv, a vector called as a function of its index, peek / pop on
;; vectors (the end) and lists (the front) and nil, ==, the number
;; predicates, and hash consistent with = across lists and vectors.
(println [(mapv inc [1 2 3]) (mapv + [1 2] [10 20]) (filterv odd? (range 6)) ([10 20 30] 1)
          (peek [1 2 3]) (pop [1 2 3]) (peek '(1 2)) (pop '(1 2)) (peek nil) (pop nil)
          (== 1 1.0 1) (== 1 2) (number? 1.5) (integer? 12345678901234567890) (integer? 1.0)
          (float? 1.5) (int? 3) (= (hash [1 2]) (hash '(1 2)))])
