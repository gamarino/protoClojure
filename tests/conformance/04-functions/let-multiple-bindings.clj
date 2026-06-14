;; EXPECT: 30
(defn f [] (let [x 10 y 20] (+ x y)))
(println (f))
