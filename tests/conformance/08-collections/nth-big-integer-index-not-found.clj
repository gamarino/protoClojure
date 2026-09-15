;; EXPECT: :none :none :none
;; With a not-found value, an out-of-bounds index of any magnitude returns it.
(println (nth [1 2 3] 12345678901234567890 :none) (nth (list 1 2) -12345678901234567890 :none) (nth [1 2 3] 9007199254740993 :none))
