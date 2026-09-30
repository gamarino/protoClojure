;; EXPECT: IllegalArgumentException
;; copy's output must be a file or a writer, not a string (as in clojure.java.io).
(println (try (copy "text" "out.txt") (catch IllegalArgumentException e "IllegalArgumentException")))
