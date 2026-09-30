;; EXPECT: :info
(println (try (throw (ex-info "x" {}))
              (catch ArithmeticException e :arith)
              (catch ClassCastException e :cce)
              (catch ExceptionInfo e :info)
              (catch Exception e :exception)))
