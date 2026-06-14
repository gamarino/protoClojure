;; EXPECT: 123
(defn outer [a]
  (fn [b]
    (fn [c] (+ a b c))))
(println (((outer 100) 20) 3))
