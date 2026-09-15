;; EXPECT-ERROR: ArithmeticException: Divide by zero
;; A zero divisor later in a variadic division raises the same error.
(println (apply / (list 7 1 0)))
