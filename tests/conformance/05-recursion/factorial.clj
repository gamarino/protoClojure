;; EXPECT: 2432902008176640000
(defn factorial [n]
  (loop [n n acc 1]
    (if (<= n 1)
      acc
      (recur (dec n) (* acc n)))))
(println (factorial 20))
