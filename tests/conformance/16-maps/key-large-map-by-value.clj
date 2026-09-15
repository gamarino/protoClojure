;; EXPECT: 4950
;; 100 vector keys stored, then every entry looked up with an equal list
;; built separately.
(defn fill [m i]
  (if (= i 100) m (recur (assoc m [i (* i i)] i) (inc i))))
(defn total [m i acc]
  (if (= i 100) acc (recur m (inc i) (+ acc (get m (list i (* i i)))))))
(println (total (fill {} 0) 0 0))
