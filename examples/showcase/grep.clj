;; grep over standard input: count the ERROR lines whose latency has four
;; digits or more, and sum those latencies. One pass, one line at a time:
;; the stream is never held in memory. Runs unchanged under protoclj and bb.
;;
;;   protoclj grep.clj < access.log
;;   bb grep.clj < access.log
(require '[clojure.java.io :as io])

(def slow-error #"ERROR .* latency=(\d{4,})ms")

(let [[n total] (reduce (fn [[n total] line]
                          (if-let [[_ ms] (re-find slow-error line)]
                            [(inc n) (+ total (parse-long ms))]
                            [n total]))
                        [0 0]
                        (line-seq (io/reader *in*)))]
  (println "matches" n "latency-ms-total" total))
