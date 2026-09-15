;; EXPECT: 10000 nil :done
;; A future thread gets the same stack depth as the main thread, and a
;; recursion that exhausts it raises StackOverflowError on that thread
;; instead of crashing the process. A future whose body throws currently
;; realises to nil (STATUS.md, Known issues).
(defn depth [n] (if (= n 0) 0 (+ 1 (depth (- n 1)))))
(defn forever [n] (+ 1 (forever n)))
(println @(future (depth 10000)) @(future (forever 0)) :done)
