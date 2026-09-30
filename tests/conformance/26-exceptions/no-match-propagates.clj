;; EXPECT: outer caught: x
;; An exception no clause matches propagates to the enclosing try.
(try
  (try (throw (ex-info "x" {}))
       (catch ArithmeticException e (println "wrong clause")))
  (catch ExceptionInfo e (println "outer caught:" (ex-message e))))
