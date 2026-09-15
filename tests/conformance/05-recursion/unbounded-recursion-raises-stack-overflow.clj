;; EXPECT-ERROR: StackOverflowError
;; Recursion that never terminates exhausts the thread's stack. It must stop
;; with a StackOverflowError runtime error, not crash the process.
(defn forever [n] (+ 1 (forever n)))
(println "before")
(forever 0)
(println "not reached")
