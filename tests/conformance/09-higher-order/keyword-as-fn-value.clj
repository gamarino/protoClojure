;; EXPECT: (1 2) (1) 3 4 5 (6 7) 3
;; Keywords are function values: higher-order primitives, locals, globals,
;; apply and pmap call them like any other function; so is a quoted symbol.
(def by-n :n)
(defn pick [k m] (k m))
(println (map :a [{:a 1} {:a 2}])
         (map :n (filter :ok [{:ok true :n 1} {:n 2}]))
         (pick :n {:n 3})
         (by-n {:n 4})
         (apply :a (list {:a 5}))
         (pmap :a [{:a 6} {:a 7}])
         ((quote s) (hash-map (quote s) 3)))
