;; throw_across5: throw 5 frames below the handler, 100000 times.
(defn deep [d] (if (= d 0) (throw (ex-info "deep" {})) (+ 1 (deep (dec d)))))
(defn run [n] (loop [i 0 c 0] (if (< i n) (recur (inc i) (+ c (try (deep 5) (catch Exception e 1)))) c)))
(println "CHECK" (run 100000))
