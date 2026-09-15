;; EXPECT: (150000 150000)
;; Regression: `pmap` joins its worker threads in order. The joins must not
;; stall a garbage-collection cycle requested by the workers' allocations.
;; Before the fix the main thread was blocked in a join outside any
;; safepoint and the program hung.
(defn build [n]
  (loop [i 0 acc (list)]
    (if (< i n) (recur (+ i 1) (cons i acc)) (count acc))))
(println (pmap build (list 150000 150000)))
