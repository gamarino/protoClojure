;; EXPECT-ERROR: ExecutionException: StackOverflowError:
;; A recursion that exhausts a future thread's stack raises StackOverflowError
;; on that thread instead of crashing the process, and deref raises it,
;; wrapped as JVM Clojure's ExecutionException. It used to realise to nil.
(defn forever [n] (+ 1 (forever n)))
(println @(future (forever 0)))
