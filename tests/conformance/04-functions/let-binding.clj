;; EXPECT: 42
(defn area [w h] (let [a (* w h)] a))
(println (area 6 7))
