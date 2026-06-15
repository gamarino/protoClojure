;; EXPECT: 1
;; Once delivered, a second deliver is a no-op — the original value sticks.
(def p (promise))
(deliver p 1)
(deliver p 2)
(println @p)
