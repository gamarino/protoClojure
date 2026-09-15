;; EXPECT: true 10 20
;; The :as binding holds the map of every keyword argument.
(defn f [& {:keys [b c] :as opts}]
  (println (= opts {:b 10 :c 20}) (:b opts) (:c opts)))
(f :b 10 :c 20)
