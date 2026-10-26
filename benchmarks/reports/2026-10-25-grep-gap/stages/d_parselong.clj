;; Stage d: parse-long once per line on a constant string, scalar accumulator.
(require '[clojure.java.io :as io])
(println "sum" (reduce (fn [t line] (+ t (parse-long "1234"))) 0 (line-seq (io/reader *in*))))
