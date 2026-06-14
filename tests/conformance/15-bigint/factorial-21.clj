;; EXPECT: 51090942171709440000
;; 21! exceeds 2^63 (long range); protoClojure auto-promotes to LargeInteger.
(defn factorial [n]
  (loop [n n acc 1]
    (if (<= n 1) acc (recur (- n 1) (* acc n)))))
(println (factorial 21))
