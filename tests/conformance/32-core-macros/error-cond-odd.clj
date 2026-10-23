;; EXPECT-ERROR: cond requires an even number of forms
;; cond with a test and no expression is an expansion error.
(defn f [] (cond true))
