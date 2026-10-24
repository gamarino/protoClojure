;; EXPECT: #{49995000} 10000
;; Futures reading one lazy seq share its realisation: each element is
;; computed once, whatever the number of readers.
(def calls (atom 0))
(def s (map (fn [x] (swap! calls inc) x) (range 10000)))
(def fs (doall (map (fn [_] (future (reduce + s))) (range 8))))
(println (set (map deref fs)) @calls)
