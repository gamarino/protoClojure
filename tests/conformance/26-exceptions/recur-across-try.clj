;; EXPECT-ERROR: Cannot recur across try
;; As in JVM Clojure, recur may not cross a try boundary.
(defn f [n]
  (loop [i 0]
    (try (if (< i n) (recur (+ i 1)) i)
         (catch Exception e :x))))
(println (f 3))
