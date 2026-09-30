;; EXPECT: boom
(println (try (throw (ex-info "boom" {:a 1}))
              (catch ExceptionInfo e (ex-message e))))
