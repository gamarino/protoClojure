;; EXPECT: :zero 10 30 33
(defn pick
  ([] :zero)
  ([x] x)
  ([x y] (+ x y))
  ([x y & rest] (+ x y (count rest))))
(println (pick) (pick 10) (pick 10 20) (pick 10 20 30 40 50))
