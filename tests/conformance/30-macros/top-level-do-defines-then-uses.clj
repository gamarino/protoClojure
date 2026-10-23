;; EXPECT: 42
;; A top-level `do` is evaluated one form at a time, as in Clojure, so a
;; macro defined by its first form is in effect for the forms after it,
;; including when a macro expands into such a `do`.
(defmacro defconst-macro [name v]
  (list 'do (list 'defmacro name [] v) (list name)))
(defconst-macro answer 42)
(println (answer))
