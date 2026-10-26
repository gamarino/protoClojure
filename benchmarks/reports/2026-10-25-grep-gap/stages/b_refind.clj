;; Stage b: + re-find, scalar accumulator (no vector, no parse-long).
(require '[clojure.java.io :as io])
(def slow-error #"ERROR .* latency=(\d{4,})ms")
(println "matches" (reduce (fn [n line] (if (re-find slow-error line) (inc n) n))
                           0 (line-seq (io/reader *in*))))
