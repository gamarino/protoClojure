;; EXPECT: :recovered
(println @(future (try (/ 1 0) (catch ArithmeticException e :recovered))))
