;; Macro-heavy loop: cond, when and when-not in the body (C++ forms in the
;; baseline, core.clj macros in stage 6). Prints the work done.
(defn classify [i] (cond (< i 10) 0 (< i 100) 1 (< i 1000) 2 :else 3))
(defn run [n]
  (loop [i 0 acc 0]
    (if (< i n)
      (recur (+ i 1)
             (+ acc (classify (- i (* 2000 (- i i))))
                (if (when (> i 5) true) 1 0)
                (if (when-not (> i 5) true) 1 0)))
      acc)))
(println "macro-loop" (run 5000000))
