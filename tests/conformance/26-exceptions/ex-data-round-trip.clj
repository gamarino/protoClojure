;; EXPECT: {:a 1} 1
;; ex-data returns the map ex-info was given, unchanged.
(def e (try (throw (ex-info "boom" {:a 1}))
            (catch Exception e e)))
(println (ex-data e) (:a (ex-data e)))
