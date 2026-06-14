;; EXPECT: 3
(defn rest-count [a & rest] (count rest))
(println (rest-count 1 2 3 4))
