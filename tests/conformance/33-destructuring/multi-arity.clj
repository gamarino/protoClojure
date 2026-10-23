;; EXPECT: 3 6
;; Each arity of a multi-arity fn destructures its own parameters.
(defn area
  ([[w h]] (* w h))
  ([[w h] [d]] (* w h d)))
(println (area [1 3]) (area [1 2] [3]))
