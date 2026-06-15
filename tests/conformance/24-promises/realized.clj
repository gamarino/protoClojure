;; EXPECT: false true
(def p (promise))
(def r1 (realized? p))
(deliver p :now)
(println r1 (realized? p))
