;; EXPECT: (println never) 0
;; A quoted call is data: nothing runs.
(def n (atom 0))
(def form '(println "never"))
(def other '(swap! n inc))
(println form @n)
