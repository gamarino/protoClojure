;; EXPECT: 10 (2 3) nil 3
;; dorun realises for side effects and answers nil; doall answers the seq;
;; (dorun n coll) stops after n more elements (Clojure realises n + 1).
(def n (atom 0))
(dorun (map (fn [x] (swap! n inc)) (range 10)))
(def m (atom 0))
(dorun 2 (map (fn [x] (swap! m inc) x) '(1 2 3 4 5)))
(println @n (doall (map inc [1 2])) (dorun (map inc [1])) @m)
