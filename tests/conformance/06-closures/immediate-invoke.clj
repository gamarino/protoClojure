;; EXPECT: 42
(defn outer [a]
  (fn [b] (+ a b)))
(println ((outer 12) 30))
