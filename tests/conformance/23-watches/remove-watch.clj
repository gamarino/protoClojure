;; EXPECT: 1
(def a (atom 0))
(def hits (atom 0))
(add-watch a :k (fn [_ _ _ _] (swap! hits inc)))
(reset! a 99)
(remove-watch a :k)
(reset! a 100)        ;; should NOT fire watcher anymore
(println @hits)
