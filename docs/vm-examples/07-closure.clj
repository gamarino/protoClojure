(defn make-adder [n]
  (fn [x] (+ x n)))

((make-adder 5) 10)
