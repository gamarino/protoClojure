;; EXPECT-ERROR: StackOverflowError
;; Printing, comparing and hashing recurse once per level of nesting. A list
;; nested 2,000,000 levels deep exceeds the thread's stack: `str` must stop
;; with a StackOverflowError runtime error, not crash the process.
(defn nest [n] (loop [i 0 acc (list)] (if (< i n) (recur (+ i 1) (list acc)) acc)))
(def deep (nest 2000000))
(println "built")
(str deep)
