;; EXPECT: 5 6 | :a 1 :b 2 | 3 | 7 8 | nil 3
;; The core.clj binding macros take binding forms: when-let, doseq, if-let,
;; when-some and if-some; a map entry destructures as [key value].
(def out (atom []))
(when-let [[a b] [5 6]] (swap! out conj a b))
(swap! out conj "|")
(doseq [[k v] [[:a 1] [:b 2]]] (swap! out conj k v))
(swap! out conj "|")
(doseq [[_ v] {:only 3}] (swap! out conj v))
(swap! out conj "|")
(when-some [{:keys [x y]} {:x 7 :y 8}] (swap! out conj x y))
(swap! out conj "|")
(swap! out conj (if-let [[a] nil] a nil) (if-some [{n :n} {:n 3}] n 0))
(apply println @out)
