;; EXPECT: nil nil
(defn f [& {:keys [a b]}] (println a b))
(f)
