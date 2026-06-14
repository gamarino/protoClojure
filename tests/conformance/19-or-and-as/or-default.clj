;; EXPECT: 42 :meters
(defn area [w h & {:keys [unit] :or {unit :meters}}]
  (println (* w h) unit))
(area 6 7)
