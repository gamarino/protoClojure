(defn nats [n]
  (lazy-seq (cons n (nats (inc n)))))

(take 3 (nats 0))

(defn squares [n]
  (for [x (range n)] (* x x)))

(doall (squares 3))
