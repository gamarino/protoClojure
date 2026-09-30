;; EXPECT: :arith
(try @(future (/ 1 0))
     (catch java.util.concurrent.ExecutionException e
       (try (throw (ex-cause e)) (catch ArithmeticException a (println :arith)))))
