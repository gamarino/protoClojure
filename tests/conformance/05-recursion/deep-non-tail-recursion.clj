;; EXPECT: 10000
;; A non-tail self-call 10,000 levels deep, the depth JVM Clojure reaches
;; with its default thread stack. Every call nests the interpreter on the
;; native stack; this crashed with SIGSEGV below 2,000 levels.
(defn depth [n] (if (= n 0) 0 (+ 1 (depth (- n 1)))))
(println (depth 10000))
