;; EXPECT: second f
;; An exception thrown inside a catch clause is not caught by its own try's
;; clauses; the finally still runs and the new exception propagates.
(def log (atom ""))
(try
  (try (throw (ex-info "first" {}))
       (catch ExceptionInfo e (throw (ex-info "second" {})))
       (catch Exception e (println "must not run"))
       (finally (swap! log str "f")))
  (catch Exception e (println (ex-message e) @log)))
