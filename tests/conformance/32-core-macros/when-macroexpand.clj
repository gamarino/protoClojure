;; EXPECT: (if a (do b c)) (if a nil (do b)) (clojure.core/make-future (clojure.core/fn [] 1))
;; when, when-not and future are core.clj macros: macroexpand shows their
;; expansions, as in Clojure.
(println (macroexpand '(when a b c)) (macroexpand '(when-not a b)) (macroexpand '(future 1)))
