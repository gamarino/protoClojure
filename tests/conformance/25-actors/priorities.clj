;; EXPECT: 3
;; Three priority levels work; semantically all msgs land in order on
;; one actor, so the result is just the count of successful sends.
(def a (actor 0))
@(send-h a inc)
@(send-m a inc)
@(send-l a inc)
(println @a)
