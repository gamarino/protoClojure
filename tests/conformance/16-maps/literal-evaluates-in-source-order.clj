;; EXPECT: a b c x y 1 2
;; A map literal's forms are evaluated left to right in source order, as in
;; Clojure, even though a runtime map has no order and these keywords were
;; interned in the opposite order first.
(def ks [:c :b :a])
(def trace (atom ""))
(defn note [x] (swap! trace (fn [t] (if (= t "") (str x) (str t " " x)))) x)
(def m {:a (note "a") :b (note "b") :c (note "c")})
(def n {"x" (note "x") "y" (note "y") 1 (note 1) 2 (note 2)})
(println @trace)
