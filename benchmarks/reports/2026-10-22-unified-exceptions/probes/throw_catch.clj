;; throw_catch: throw an ex-info and catch it in the same function, 200000 times.
(defn run [n] (loop [i 0 c 0] (if (< i n) (recur (inc i) (+ c (try (throw (ex-info "x" {})) (catch Exception e 1)))) c)))
(println "CHECK" (run 200000))
