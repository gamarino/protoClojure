;; EXPECT: 11 12 13
(defn make-counter [start]
  (let [a (atom start)]
    (fn [] (swap! a inc))))
(def c (make-counter 10))
(println (c) (c) (c))
