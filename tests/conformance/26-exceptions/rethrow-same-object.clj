;; EXPECT: true
;; Rethrowing the caught exception propagates the same object.
(def original (ex-info "x" {:n 1}))
(println (try
           (try (throw original)
                (catch Exception e (throw e)))
           (catch Exception e2 (= e2 original))))
