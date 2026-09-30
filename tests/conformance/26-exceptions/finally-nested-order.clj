;; EXPECT: 12345
;; Nested finally blocks run innermost first while the exception unwinds.
(def log (atom ""))
(try
  (try
    (try (swap! log str "1") (throw (ex-info "x" {}))
         (finally (swap! log str "2")))
    (finally (swap! log str "3")))
  (catch Exception e (swap! log str "4"))
  (finally (swap! log str "5")))
(println @log)
