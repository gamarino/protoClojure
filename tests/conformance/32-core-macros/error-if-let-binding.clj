;; EXPECT-ERROR: if-let requires exactly 2 forms in binding vector
;; The binding macros check their binding vector at expansion time, as
;; Clojure's do, and the error names the macro.
(if-let [a 1 b 2] a)
