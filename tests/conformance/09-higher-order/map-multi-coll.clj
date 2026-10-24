;; EXPECT: (11 22) ([1 :a] [2 :b])
;; map over several collections stops at the shortest, as in Clojure.
(println (map + [1 2] [10 20 30]) (map vector [1 2 3] [:a :b]))
