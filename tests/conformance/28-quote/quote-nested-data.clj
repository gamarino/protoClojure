;; EXPECT: (a [b {:k (c d)}] s 1.5 nil true) true true
;; quote takes any datum: lists, vectors and maps, nested, and atoms inside.
(def d '(a [b {:k (c d)}] "s" 1.5 nil true))
(println d (vector? (nth d 1)) (map? (nth (nth d 1) 1)))
