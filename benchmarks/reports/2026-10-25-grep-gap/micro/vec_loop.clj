;; 1446319 steps of the grep accumulator without any I/O: `plain` carries two
;; longs, `vec` builds [n total] and destructures it each step, as grep.clj's
;; reduce does. Prints both sums and the milliseconds of each loop.
(def N 1446319)
(defn now [] (System/currentTimeMillis))
(let [t0 (now)
      r (loop [i 0 n 0 total 0] (if (< i N) (recur (+ i 1) (+ n 1) (+ total i)) [n total]))
      t1 (now)
      v (loop [i 0 acc [0 0]] (if (< i N) (let [[n total] acc] (recur (+ i 1) [(+ n 1) (+ total i)])) acc))
      t2 (now)]
  (println "plain" r (- t1 t0) "ms; vec" v (- t2 t1) "ms"))
