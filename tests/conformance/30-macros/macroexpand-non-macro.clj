;; EXPECT: (+ 1 2) 5 (if a b)
;; macroexpand and macroexpand-1 return a form that is not a macro call
;; unchanged; special forms are never expanded.
(println (macroexpand '(+ 1 2)) (macroexpand 5) (macroexpand-1 '(if a b)))
