;; EXPECT-ERROR: ExecutionException: ExecutionException: IndexOutOfBoundsException: nth index 5 is out of bounds (count 2)
;; The inner deref raises ExecutionException inside the outer future's body,
;; so the outer deref wraps it again, as in JVM Clojure.
(println @(future @(future (nth [1 2] 5))))
