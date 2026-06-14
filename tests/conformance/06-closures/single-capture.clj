;; EXPECT: 15
(defn make-adder [n]
  (fn [x] (+ x n)))
(def add5 (make-adder 5))
(println (add5 10))
