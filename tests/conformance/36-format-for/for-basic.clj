;; EXPECT: (1 4 9) ([1 :a] [1 :b] [2 :a] [2 :b]) () ([1 :a])
;; for: one value per element, nested bindings rightmost fastest, the
;; empty list for no elements, destructuring binding forms.
(println (for [x [1 2 3]] (* x x))
         (for [x [1 2] y [:a :b]] [x y])
         (for [x []] x)
         (for [[k v] {:a 1}] [v k]))
