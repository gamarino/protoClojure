;; EXPECT: nil nil nil nil
;; ex-message, ex-data and ex-cause return nil for a value that is not an
;; exception; ex-data is nil for an exception that is not an ExceptionInfo.
(println (ex-message 42) (ex-data "x") (ex-cause nil)
         (try (/ 1 0) (catch ArithmeticException e (ex-data e))))
