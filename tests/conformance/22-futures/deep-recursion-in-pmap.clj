;; EXPECT: (10000 10000)
;; pmap worker threads get the same stack depth as the main thread.
(defn depth [n] (if (= n 0) 0 (+ 1 (depth (- n 1)))))
(println (pmap depth (list 10000 10000)))
