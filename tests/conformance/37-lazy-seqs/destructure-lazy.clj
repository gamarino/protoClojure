;; EXPECT: 1 2 (3 4 5) 0 1
;; Destructuring reads a lazy seq, an endless one included.
(let [[a b & r] (map inc (range 5))
      [x y] (iterate inc 0)]
  (println a b r x y))
