;; EXPECT: [1 2] true
;; doto calls each form with the value first and returns the value.
(def a (atom []))
(def r (doto a (swap! conj 1) (swap! conj 2)))
(println @a (= r a))
