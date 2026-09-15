;; EXPECT: 300000
;; Regression: `deref` on a pending future joins its thread. The join must
;; not stall a garbage-collection cycle requested by the future's own
;; allocations. Before the fix the main thread was blocked in the join
;; outside any safepoint and the program hung.
(defn build [n]
  (loop [i 0 acc (list)]
    (if (< i n) (recur (+ i 1) (cons i acc)) (count acc))))
(def f (future (build 300000)))
(println @f)
