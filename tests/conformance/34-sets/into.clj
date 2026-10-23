;; EXPECT: true [1 2 3] {:a 1, :b 2} (3 2 1)
;; into conjoins every element of a collection onto another, as in Clojure.
(println (= (into #{} [1 2 2 3]) #{1 2 3})
         (into [1] [2 3])
         (into {:a 1} [[:b 2]])
         (into () [1 2 3]))
