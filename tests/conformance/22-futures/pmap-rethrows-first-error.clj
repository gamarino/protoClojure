;; EXPECT-ERROR: ExecutionException: IndexOutOfBoundsException: nth index 3 is out of bounds (count 0)
;; pmap raises the error of the first failing element in input order, as
;; consuming JVM Clojure's pmap does. Failing elements used to map to nil.
(println (pmap (fn [x] (cond (= x 1) (nth [] 3)
                             (= x 2) (/ 1 0)
                             :else x))
               (list 0 1 2 3)))
