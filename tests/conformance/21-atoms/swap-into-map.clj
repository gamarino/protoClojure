;; EXPECT: true 1
(def stats (atom {:hits 0 :miss 0}))
(swap! stats assoc :hits 1)
(println (= @stats {:hits 1 :miss 0}) (:hits @stats))
