;; EXPECT: 6 [] 0
;; apply spreads a vector, and nil spreads no arguments.
(println (apply + [1 2 3]) (apply vector nil) (apply + nil))
