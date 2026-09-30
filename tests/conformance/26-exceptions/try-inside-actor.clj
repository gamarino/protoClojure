;; EXPECT: 1
;; A handler that catches its own exception keeps the actor's state.
(def a (actor 0))
@(send a (fn [s] (try (/ s 0) (catch ArithmeticException e (+ s 1)))))
(println @a)
