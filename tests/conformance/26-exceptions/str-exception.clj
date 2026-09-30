;; EXPECT: ExceptionInfo: boom {:a 1}|ArithmeticException: Divide by zero
;; str of an exception is its toString: the class, the message and, for an
;; ExceptionInfo, the data.
(println (str (ex-info "boom" {:a 1}) "|" (try (/ 1 0) (catch Exception e e))))
