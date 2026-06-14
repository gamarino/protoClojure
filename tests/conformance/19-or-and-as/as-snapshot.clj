;; EXPECT: {:b 10, :c 20}
(defn f [& {:keys [b c] :as opts}] (println opts))
(f :b 10 :c 20)
