;; EXPECT: [:a 1 :a 2]
;; A repeated keyword in the arguments of an ordinary callee is an ordinary
;; argument: nothing is de-duplicated (it used to print [:a 2]).
(println (vector :a 1 :a 2))
