;; EXPECT: 3 8
;; `defmacro` and `defn` accept a docstring and an attribute map after the
;; name, as in Clojure; both are dropped (there are no vars to hold them).
(defmacro first-of "The first argument." {:added "1.0"} [x & _] x)
(defn twice "Doubles n." [n] (* 2 n))
(println (first-of 3 4 5) (twice 4))
