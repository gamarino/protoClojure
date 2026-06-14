;; EXPECT: {:hits 1, :miss 0}
(def stats (atom {:hits 0 :miss 0}))
(swap! stats assoc :hits 1)
(println @stats)
