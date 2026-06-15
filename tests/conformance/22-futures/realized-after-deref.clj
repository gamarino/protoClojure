;; EXPECT: 42 true
(def f (future 42))
(def v @f)
(println v (realized? f))
