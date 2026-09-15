;; EXPECT: nil nil nil
;; A nil global resolves inside a function body and on a future thread.
(def x nil)
(defn read-x [] x)
(println (read-x) @(future x) (apply read-x (list)))
