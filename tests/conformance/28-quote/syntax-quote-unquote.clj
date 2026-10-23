;; EXPECT: (a 2 [3 4]) (a 2 3 4) [x 2 3 4] {:v 2}
;; ~ inserts a value, ~@ splices a sequence, inside lists, vectors and maps.
(def b 2)
(def c [3 4])
(println `(a ~b ~c) `(a ~b ~@c) `[x ~b ~@c] `{:v ~b})
