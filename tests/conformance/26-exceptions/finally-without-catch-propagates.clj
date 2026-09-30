;; EXPECT: fo x
;; try/finally with no catch runs the finally and lets the exception go on.
(def log (atom ""))
(try
  (try (throw (ex-info "x" {}))
       (finally (swap! log str "f")))
  (catch Exception e (swap! log str "o") (println @log (ex-message e))))
