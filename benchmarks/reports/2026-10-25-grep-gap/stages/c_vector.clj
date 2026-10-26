;; Stage c: vector accumulator [n total] destructured each step, no regex.
(require '[clojure.java.io :as io])
(let [[n total] (reduce (fn [[n total] line] [(inc n) (+ total (count line))])
                        [0 0] (line-seq (io/reader *in*)))]
  (println "lines" n "chars" total))
