;; EXPECT: caught RuntimeException
;; A form `eval` cannot compile raises a RuntimeException the program can
;; catch ("Syntax error compiling: ...").
(defmacro one [x] x)
(println (try (eval '(one)) (catch RuntimeException e (str "caught " "RuntimeException"))))
