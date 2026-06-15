;; EXPECT: 50
(def a (actor 0))
@(send a + 10 20 20)
(println @a)
