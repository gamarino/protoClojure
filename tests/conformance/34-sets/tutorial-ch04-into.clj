;; EXPECT: [2 3 4] true
;; docs/tutorial/04-data-structures.md §4.7: the into idiom over a map seq.
(println (vec (map inc [1 2 3]))
         (= (into {} (map (fn [[k v]] [k (inc v)]) {:a 1 :b 2})) {:a 2 :b 3}))
