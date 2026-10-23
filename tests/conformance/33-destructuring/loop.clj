;; EXPECT: 6 [3 2 1]
;; loop destructures its bindings on entry and on every recur.
(println
  (loop [[x & xs] [1 2 3] acc 0] (if x (recur xs (+ acc x)) acc))
  (loop [[x & xs :as all] [1 2 3] out []] (if (seq all) (recur xs (cons x out)) (vec out))))
