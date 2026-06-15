;; EXPECT: (100 400 900 1600)
;; Until pmap goes parallel, this is the explicit shape users can write
;; today to get real multi-core execution.
(defn sq [x] (* x x))
(def fs (map (fn [x] (future (sq x))) (list 10 20 30 40)))
(println (map deref fs))
