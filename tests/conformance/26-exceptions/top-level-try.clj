;; EXPECT: top-level: Divide by zero
;; try works at the top level of a file, outside any fn.
(try (/ 1 0)
     (catch ArithmeticException e (println "top-level:" (ex-message e))))
