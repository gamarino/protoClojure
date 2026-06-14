;; EXPECT: nil
(defn f [& {:keys [b] :as opts}] (println opts))
(f)
