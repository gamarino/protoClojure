;; EXPECT: nil 10000
;; Actor worker threads get the same stack depth as the main thread. A
;; message whose handler exhausts the stack raises StackOverflowError on the
;; worker, which survives it and handles the next message; the failed
;; message currently sets the actor's value to nil (STATUS.md, Known issues).
(defn depth [n] (if (= n 0) 0 (+ 1 (depth (- n 1)))))
(defn forever [n] (+ 1 (forever n)))
(def a (actor 0))
(println @(send a (fn [_] (forever 0))) @(send a (fn [_] (depth 10000))))
