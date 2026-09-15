;; EXPECT: :done
;; Regression: a `loop` that allocates nothing must still let a
;; garbage-collection cycle requested by another thread start. The main
;; thread spins on SmallInt opcodes until the future, after allocating
;; enough to request a cycle, sets `stop`. Before the fix the loop never
;; reached a safepoint, the cycle never started, the future stayed parked
;; and the program hung.
(defn build [n]
  (loop [i 0 acc (list)]
    (if (< i n) (recur (+ i 1) (cons i acc)) (count acc))))
(def stop 0)
(def f (future (do (build 300000) (def stop 1))))
(defn spin []
  (loop [i 0]
    (if (= stop 0) (recur (+ i 1)) :done)))
(println (spin))
