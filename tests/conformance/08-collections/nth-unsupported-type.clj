;; EXPECT-ERROR: UnsupportedOperationException: nth not supported on a map
;; nth on a value that is not sequential, a string or nil raises the analogue
;; of JVM Clojure's UnsupportedOperationException, naming the type (it was
;; "seq op: argument is not a list or vector").
(println (nth {:a 1} 0))
