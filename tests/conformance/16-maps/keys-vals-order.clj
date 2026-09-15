;; EXPECT: (:z :y :x) (1 2 3)
;; keys and vals walk the map in insertion order.
(println (keys {:z 1 :y 2 :x 3}) (vals {:z 1 :y 2 :x 3}))
