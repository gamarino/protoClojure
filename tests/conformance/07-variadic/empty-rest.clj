;; EXPECT: 7 ()
(defn show [a & rest] (println a rest))
(show 7)
