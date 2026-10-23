;; EXPECT: (+ 1 2) 3 + (1 2) true 2
;; A quoted list is a list value: the sequence functions take it apart.
(def code '(+ 1 2))
(println code (count code) (first code) (rest code) (list? code) (nth code 2))
