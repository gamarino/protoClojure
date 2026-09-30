;; EXPECT: 6 :bad
(defn safe-div [a b]
  (let [r (try (/ a b) (catch ArithmeticException e :bad))]
    r))
(println (safe-div 12 2) (safe-div 1 0))
