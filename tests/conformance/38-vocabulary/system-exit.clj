;; EXPECT-ERROR
;; (System/exit n) exits with status n, as in Clojure (and as `exit` does).
(println "bye")
(System/exit 3)
