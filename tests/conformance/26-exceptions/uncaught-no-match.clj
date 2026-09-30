;; EXPECT-ERROR: runtime error: ArithmeticException: Divide by zero
(try (/ 1 0) (catch ClassCastException e :no))
