;; EXPECT-ERROR: splice (~@) not in list
(def x [1])
(println `~@x)
