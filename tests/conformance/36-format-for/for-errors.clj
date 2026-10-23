;; EXPECT: true true true
;; for checks its binding vector when it is expanded, with Clojure's
;; messages (eval wraps them in a compile error).
(defn err [form] (try (eval form) (catch Exception e (ex-message e))))
(println (includes? (err '(for [x] x)) "for requires an even number of forms in binding vector")
         (includes? (err '(for [x [1] :until true] x)) "Invalid 'for' keyword :until")
         (includes? (err '(for [:when true x [1]] x)) "for binding must start with a binding form, not :when"))
