;; Stage a: line-seq + count only.
(require '[clojure.java.io :as io])
(println "lines" (count (line-seq (io/reader *in*))))
