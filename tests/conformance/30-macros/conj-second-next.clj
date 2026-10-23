;; EXPECT: (0 1 2) [1 2 3] {:b 2} 2 nil (2 3) "a" :b
;; Sequence helpers macros reach for: conj adds where the collection grows,
;; second and next walk a form, pr-str prints readably.
(println (conj '(1 2) 0) (conj [1 2] 3) (conj {} [:b 2]) (second '(1 2 3)) (next '(1))
         (next [1 2 3]) (pr-str "a") (last [:a :b]))
