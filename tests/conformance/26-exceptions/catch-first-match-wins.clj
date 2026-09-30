;; EXPECT: :arith
;; Clauses are tried in order; the first whose class matches runs, even when
;; a later clause would match too.
(println (try (/ 1 0)
              (catch ArithmeticException e :arith)
              (catch Exception e :exception)))
