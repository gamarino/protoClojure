;; EXPECT-ERROR: Don't know how to create ISeq from: an integer
;; The error is raised when the lazy seq is read, as in Clojure.
(def s (map inc 5))
(println (first s))
