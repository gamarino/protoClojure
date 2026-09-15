;; EXPECT: true
;; A promise delivered on another thread turns realized? true only once its
;; value can be read: a thread polling realized? always reads the delivered
;; value, never nil.
(defn wait-realized [p] (if (realized? p) @p (recur p)))
(defn check [i n]
  (if (= i n)
    true
    (let [p (promise)]
      (future (deliver p i))
      (if (= (wait-realized p) i) (recur (inc i) n) false))))
(println (check 0 500))
