;; EXPECT: 2
(def a (atom 0))
(def n (atom 0))
(add-watch a :a (fn [_ _ _ _] (swap! n inc)))
(add-watch a :b (fn [_ _ _ _] (swap! n inc)))
(reset! a 1)         ;; both watchers fire
(println @n)
