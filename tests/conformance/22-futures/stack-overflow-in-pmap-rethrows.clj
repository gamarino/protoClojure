;; EXPECT-ERROR: ExecutionException: StackOverflowError:
;; A recursion that exhausts a pmap worker's stack raises StackOverflowError
;; on that worker instead of crashing the process, and pmap raises it,
;; wrapped as ExecutionException. The element used to map to nil.
(defn depth [n] (if (= n 0) 0 (+ 1 (depth (- n 1)))))
(defn forever [n] (+ 1 (forever n)))
(println (pmap (fn [n] (if (< n 0) (forever 0) (depth n))) (list 10000 -1 10000)))
