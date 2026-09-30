;; EXPECT: Divide by zero
;; An error raised by a primitive is an exception of the class its message
;; names; ex-message is the text after the class name.
(println (try (/ 1 0) (catch ArithmeticException e (ex-message e))))
