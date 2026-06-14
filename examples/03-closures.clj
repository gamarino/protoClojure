(defn make-adder [n]
  (fn [x] (+ x n)))

(def add5 (make-adder 5))
(def add100 (make-adder 100))

(println (add5 10))
(println (add100 23))

(defn compose [f g]
  (fn [x] (f (g x))))
(defn inc1 [x] (+ x 1))
(defn dbl  [x] (* x 2))

(println ((compose inc1 dbl) 10))
