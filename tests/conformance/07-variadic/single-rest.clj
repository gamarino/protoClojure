;; EXPECT: 1 (2 3 4)
(defn show [a & rest] (println a rest))
(show 1 2 3 4)
