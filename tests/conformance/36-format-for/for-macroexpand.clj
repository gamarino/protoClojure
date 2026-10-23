;; EXPECT: (clojure.core/__for-step xs (fn [x] (clojure.core/list x)))
;; for expands into one __for-step per binding (the hook stage 11 makes lazy).
(println (macroexpand '(for [x xs] x)))
