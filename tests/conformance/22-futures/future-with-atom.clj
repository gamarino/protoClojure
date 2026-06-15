;; EXPECT: 99
;; A future writing to an atom — exercises the cross-thread CAS path.
(def a (atom 0))
(def f (future (reset! a 99)))
@f
(println @a)
