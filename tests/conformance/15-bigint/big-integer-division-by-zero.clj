;; EXPECT-ERROR: ArithmeticException: Divide by zero
;; A big integer divided by zero raises the same error as a SmallInteger.
(println (/ 12345678901234567890 0))
