;; EXPECT: 0 1 2 3 | [1 3] [2 3] | :a 1 | nil
;; doseq takes :let, :when and :while, as for does, and answers nil.
(def out (atom []))
(def r (doseq [x (range 5) :when (even? x) :let [y (inc x)] :while (< x 3)] (swap! out conj x y)))
(swap! out conj "|")
(doseq [x [1 2] y [3 4] :when (not= y 4)] (swap! out conj [x y]))
(swap! out conj "|")
(doseq [[k v] {:a 1} :let [w v]] (swap! out conj k w))
(swap! out conj "|" r)
(apply println @out)
