;; Maps: literal, hash-map, assoc, get, contains?, keys, vals.
(println {:a 1 :b 2 :c 3})
(println (hash-map :x 10 :y 20))
(println (get {:a 1 :b 2} :b))
(println (assoc {:a 1} :b 2 :c 3))
(println (contains? {:a 1 :b 2} :a))
(println (keys {:a 1 :b 2 :c 3}))
(println (vals {:a 1 :b 2 :c 3}))

;; Named-argument destructuring: `& {:keys [...]}` on the callee side,
;; trailing map literal on the call site. The Clojure idiom `(f a :k v)`
;; (kv pairs without a map literal) lands in session 14.
(defn area [w h & {:keys [unit]}]
  (println "area =" (* w h) "in" unit))
(area 6 7)
(area 6 7 {:unit :meters})
(area 6 7 {:unit :feet})
