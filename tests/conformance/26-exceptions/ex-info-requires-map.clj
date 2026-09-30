;; EXPECT: IllegalArgumentException
;; JVM Clojure: "Additional data must be non-nil." — an IllegalArgumentException.
(println (try (ex-info "x" nil)
              (catch IllegalArgumentException e "IllegalArgumentException")))
