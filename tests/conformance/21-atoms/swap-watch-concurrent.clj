;; EXPECT: 400 true
;; Concurrent swap! calls on one atom from four threads: every increment
;; lands, and every watch call receives the old and new values of its own
;; swap!, the new one exactly one above the old one.
(def a (atom 0))
(def bad (atom 0))
(add-watch a :check (fn [k r o n] (if (= n (inc o)) nil (swap! bad inc))))
(defn bump [n] (if (= n 0) :done (do (swap! a inc) (recur (dec n)))))
(pmap (fn [x] (bump 100)) [1 2 3 4])
(println @a (= @bad 0))
