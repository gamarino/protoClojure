;; EXPECT: 1 2 4 :nf 5 5 (2 3 4 5) nil true
;; first, second, nth, last, count, rest, next and empty? read lazy seqs.
(def s (map inc (range 5)))
(println (first s) (second s) (nth s 3) (nth s 10 :nf) (last s) (count s)
         (rest s) (next (take 1 s)) (empty? (filter neg? s)))
