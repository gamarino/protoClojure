;; EXPECT: outer inner {:level 2}
;; The three-argument ex-info records a cause, which ex-cause returns.
(defn f []
  (try (throw (ex-info "inner" {:level 2}))
       (catch ExceptionInfo e
         (throw (ex-info "outer" {:level 1} e)))))
(try (f)
     (catch ExceptionInfo e
       (println (ex-message e) (ex-message (ex-cause e)) (ex-data (ex-cause e)))))
