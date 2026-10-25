;; EXPECT: 2 -5 2 5 false 8 15 -4 8 8 false true 2x3
;; The operators the compiler inlines (+ - * < <= > >= =) honour a
;; redefinition of their global: after `(def + -)`, `(+ 5 3)` is 2, as
;; `(+ 5)` and `apply` already were, while `clojure.core/+` and a local
;; binding are unaffected. Every value checked against Babashka 1.12.200.
(def + -)
(def r1 [(+ 5 3) (+ 5) (apply + [5 3]) (+ 10 3 2)])
(def < >)
(def r2 (< 1 2))
(def r3 [(clojure.core/+ 5 3) (let [+ clojure.core/*] (+ 5 3)) (+ 1 2 3)])
(def + clojure.core/+)
(def r4 [(+ 5 3) (+ 4 4)])
(def = not=)
(def r5 [(= 1 1) (= 1 2)])
(def * (fn [a b] (str a "x" b)))
(println (r1 0) (r1 1) (r1 2) (r1 3) r2 (r3 0) (r3 1) (r3 2) (r4 0) (r4 1) (r5 0) (r5 1) (* 2 3))
