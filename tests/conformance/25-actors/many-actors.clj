;; EXPECT: (1 4 9 16 25)
;; Five actors, each computing a different square in parallel.
(defn sq [x] (* x x))
(def actors (map (fn [n] (actor n)) (list 1 2 3 4 5)))
(def fs (map (fn [a] (send a sq)) actors))
(map deref fs)
(println (map deref actors))
