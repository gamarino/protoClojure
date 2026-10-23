;; EXPECT: 1 2 (3 4) [1 2 3 4] | nil nil | 1 nil
;; Positional destructuring in let: & binds the rest as a seq, :as the whole
;; value; missing positions are nil, as in Clojure.
(let [[a b & r :as all] [1 2 3 4]
      [c d] nil
      [e f] '(1)]
  (println a b r all "|" c d "|" e f))
