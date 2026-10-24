;; EXPECT: (clojure.core/__for-step xs (fn [x] (clojure.core/list x)) true)
;; for expands into one __for-step per binding; the innermost passes true (it may walk chunks).
(println (macroexpand '(for [x xs] x)))
