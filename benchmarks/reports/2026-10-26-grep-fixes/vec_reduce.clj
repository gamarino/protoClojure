;; The accumulator through reduce over (range N), no I/O: scalar against the
;; [n total] vector destructured in the fn's parameter, as in grep.clj.
(def N 1446319)
(defn now [] (System/currentTimeMillis))
(let [t0 (now)
      s (reduce (fn [t i] (+ t i)) 0 (range N))
      t1 (now)
      v (reduce (fn [[n total] i] [(inc n) (+ total i)]) [0 0] (range N))
      t2 (now)]
  (println "scalar" s (- t1 t0) "ms; vec" v (- t2 t1) "ms"))
