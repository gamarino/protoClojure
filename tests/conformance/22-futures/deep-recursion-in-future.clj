;; EXPECT: 10000 :done
;; A future thread gets the same stack depth as the main thread.
(defn depth [n] (if (= n 0) 0 (+ 1 (depth (- n 1)))))
(println @(future (depth 10000)) :done)
