;; EXPECT: (10000 nil 10000)
;; pmap worker threads get the same stack depth as the main thread, and a
;; recursion that exhausts one raises StackOverflowError on that worker
;; instead of crashing the process. An element whose call throws currently
;; maps to nil (STATUS.md, Known issues).
(defn depth [n] (if (= n 0) 0 (+ 1 (depth (- n 1)))))
(defn forever [n] (+ 1 (forever n)))
(println (pmap (fn [n] (if (< n 0) (forever 0) (depth n))) (list 10000 -1 10000)))
