;; EXPECT: 1
(def a (actor 0))
@(send a inc)
(println @a)
