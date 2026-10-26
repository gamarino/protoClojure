;; Stage c2: as c_vector, with (+ n 1) for (inc n): one CALL fewer per step
;; (ADD is an opcode, inc a call), to price one primitive call.
(require '[clojure.java.io :as io])
(let [[n total] (reduce (fn [[n total] line] [(+ n 1) (+ total (count line))])
                        [0 0] (line-seq (io/reader *in*)))]
  (println "lines" n "chars" total))
