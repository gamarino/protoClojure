;; EXPECT: 10 ab (1) [1 2]
;; (apply f x y coll) spreads the leading arguments and then the collection,
;; as in Clojure.
(println (apply + 1 2 [3 4]) (apply str "a" ["b"]) (apply list 1 nil) (apply vector 1 #{2}))
