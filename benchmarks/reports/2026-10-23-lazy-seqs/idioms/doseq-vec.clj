(def v (vec (range 300000)))
(def n (atom 0))
(doseq [x v] (swap! n + x))
(println @n)
