;; EXPECT: [1 {:k (2 3)}]({:a [1]})
;; str renders collections nested in collections, maps included, the way
;; println prints them.
(println (str [1 {:k (list 2 3)}] (list {:a [1]})))
