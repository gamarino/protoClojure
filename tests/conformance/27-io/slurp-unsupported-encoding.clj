;; EXPECT: IllegalArgumentException
;; Only UTF-8 is supported.
(println (try (slurp "x.txt" :encoding "ISO-8859-1")
              (catch IllegalArgumentException e "IllegalArgumentException")))
