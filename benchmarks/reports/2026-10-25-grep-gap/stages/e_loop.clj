;; Stage e: the full grep with loop/recur and two locals instead of a vector.
(require '[clojure.java.io :as io])
(def slow-error #"ERROR .* latency=(\d{4,})ms")
(loop [ls (line-seq (io/reader *in*)) n 0 total 0]
  (if-let [line (first ls)]
    (if-let [[_ ms] (re-find slow-error line)]
      (recur (next ls) (inc n) (+ total (parse-long ms)))
      (recur (next ls) n total))
    (println "matches" n "latency-ms-total" total)))
