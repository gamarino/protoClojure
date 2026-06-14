;; EXPECT: 21
(defn compose [f g]
  (fn [x] (f (g x))))
(defn inc1 [x] (+ x 1))
(defn dbl  [x] (* x 2))
(def h (compose inc1 dbl))
(println (h 10))
