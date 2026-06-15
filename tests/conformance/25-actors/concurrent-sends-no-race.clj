;; EXPECT: 100
;; 100 concurrent sends on the same actor — the single-method
;; invariant guarantees no message is lost or interleaved, so
;; the final counter equals exactly the number of sends.
(defn run []
  (let [counter (actor 0)
        fs (loop [i 0 acc (list)]
             (if (>= i 100)
               acc
               (recur (+ i 1) (cons (send counter inc) acc))))]
    (map deref fs)
    @counter))
(println (run))
