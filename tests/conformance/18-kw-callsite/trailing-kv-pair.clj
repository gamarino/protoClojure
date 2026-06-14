;; EXPECT: area = 42 in :feet
(defn area [w h & {:keys [unit]}]
  (println "area =" (* w h) "in" unit))
(area 6 7 :unit :feet)
