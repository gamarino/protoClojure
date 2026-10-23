;; EXPECT: [0 1 2] nil 0
;; dotimes binds the counter from 0 to n-1; zero times runs nothing.
(def seen (atom []))
(def r (dotimes [i 3] (swap! seen conj i)))
(def none (atom 0))
(dotimes [i 0] (swap! none inc))
(println @seen r @none)
