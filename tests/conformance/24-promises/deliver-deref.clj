;; EXPECT: ready
(def p (promise))
(def f (future (deliver p "ready")))
@f
(println @p)
