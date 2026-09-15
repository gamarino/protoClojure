;; EXPECT-ERROR: StringIndexOutOfBoundsException: nth index 3 is out of bounds (count 3)
;; An index past the end of a string raises the analogue of JVM Clojure's
;; StringIndexOutOfBoundsException.
(println (nth "abc" 3))
