;; EXPECT: captured
;; The catch binding is a local like any other; a closure can capture it.
(defn f []
  (try (throw (ex-info "captured" {}))
       (catch Exception e (fn [] (ex-message e)))))
(println ((f)))
