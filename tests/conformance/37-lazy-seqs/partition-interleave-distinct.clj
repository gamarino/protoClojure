;; EXPECT: ((0 1) (2 3) (4 5)) ((0 1 2) (3 4 5) (6 :a :b)) ((0 1 2) (3 4 5) (6)) ((0 1) (2 3)) (1 :a 2 :b) (1 2 3)
;; partition (with step and pad), partition-all, interleave, distinct.
(println (partition 2 (range 7)) (partition 3 3 [:a :b] (range 7)) (partition-all 3 (range 7))
         (take 2 (partition 2 (range))) (interleave [1 2 3] [:a :b]) (distinct [1 2 1 3 2]))
