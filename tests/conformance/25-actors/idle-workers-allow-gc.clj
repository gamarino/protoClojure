;; EXPECT: 300000
;; Regression: an idle actor worker blocked waiting for work must not stall
;; a garbage-collection cycle requested by another thread. Starting the
;; actor pool leaves most workers idle; the main thread then allocates
;; enough to trigger a cycle. Before the fix the idle workers never reached
;; a safepoint and the program hung.
(defn build [n]
  (loop [i 0 acc (list)]
    (if (< i n) (recur (+ i 1) (cons i acc)) (count acc))))
(def a (actor 0))
(send a inc)
(println (build 300000))
