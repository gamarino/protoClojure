;; EXPECT: #error {:type ExceptionInfo, :message "outer", :data {:n 1}, :cause #error {:type ArithmeticException, :message "Divide by zero"}}
;; The cause prints nested under :cause.
(println (ex-info "outer" {:n 1} (try (/ 1 0) (catch Exception e e))))
