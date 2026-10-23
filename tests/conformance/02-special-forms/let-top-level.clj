;; EXPECT: 3 1 10
;; `let` and `loop` work at the top level of a file: such a form runs as the
;; body of a fn called on the spot. An inner binding shadows an outer one only
;; inside its own body.
(def total (loop [i 0 acc 0] (if (< i 3) (recur (+ i 1) (+ acc 1)) acc)))
(let [a 1 b (let [a 10] a)]
  (let [a 2] a)
  (println total a b))
