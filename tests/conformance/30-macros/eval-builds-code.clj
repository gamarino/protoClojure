;; EXPECT: 6 [1 2] :k
;; `eval` compiles and runs a form built as data, as a top-level form.
(def code (list '+ 1 2 3))
(println (eval code) (eval [1 '(inc 1)]) (eval :k))
