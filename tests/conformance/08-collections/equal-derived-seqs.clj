;; EXPECT: true true true true true
;; Sequences returned by rest, map, filter, keys and cons compare by value
;; with vectors and lists.
(println (= (rest [0 1 2]) [1 2])
         (= (map inc [1 2]) [2 3])
         (= (filter (fn [x] (> x 1)) (list 1 2 3)) [2 3])
         (= (keys {:a 1 :b 2}) [:a :b])
         (= (cons 0 [1]) (list 0 1)))
