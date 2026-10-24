;; EXPECT: (0 1 2) (3 4 5) (0 1 2) (3 4 -1 5) () (0 1 2 3 4 5)
;; take, drop, take-while and drop-while, over endless seqs too.
(println (take 3 (range)) (take 3 (drop 3 (range)))
         (take-while (fn [x] (< x 3)) (range)) (drop-while (fn [x] (< x 3)) [0 1 2 3 4 -1 5])
         (take 0 [1]) (take 10 (range 6)))
