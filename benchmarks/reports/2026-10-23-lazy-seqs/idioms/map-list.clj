(def l (apply list (range 100000)))
(println (reduce + (map (fn [x] (* x x)) l)))
