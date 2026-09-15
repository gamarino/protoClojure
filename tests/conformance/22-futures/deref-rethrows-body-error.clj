;; EXPECT-ERROR: ExecutionException: ArithmeticException: Divide by zero
;; deref of a future whose body raised an error raises it, wrapped as JVM
;; Clojure's java.util.concurrent.ExecutionException. It used to return nil.
(def f (future (/ 1 0)))
(println "deref returned" @f)
