;; Stage a2: reduce with a scalar accumulator, one fn call per line.
(require '[clojure.java.io :as io])
(println "lines" (reduce (fn [n line] (inc n)) 0 (line-seq (io/reader *in*))))
