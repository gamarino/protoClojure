;; EXPECT: 265252859812191058636308480000000
(defn factorial [n]
  (loop [n n acc 1]
    (if (<= n 1) acc (recur (- n 1) (* acc n)))))
(println (factorial 30))
