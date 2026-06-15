;; Fan-out throughput. 1000 actors × 100 inc messages each = 100_000
;; total. With many actors, the scheduler distributes work across the
;; worker pool and we should see wall-clock scaling roughly with
;; min(workers, ACTORS).

(defn drain [futures]
  (loop [fs futures]
    (if (empty? fs)
      :done
      (do @(first fs)
          (recur (rest fs))))))

(defn run []
  (let [ACTORS 1000
        MSGS-EACH 1000
        actors
        (loop [i 0 acc (list)]
          (if (>= i ACTORS)
            acc
            (recur (+ i 1) (cons (actor 0) acc))))
        ;; For each actor, fire MSGS-EACH sends; remember just the last
        ;; promise per actor — single-method invariant guarantees the
        ;; earlier sends are already done when the last completes.
        last-fs
        (loop [as actors fs (list)]
          (if (empty? as)
            fs
            (let [a (first as)
                  this-last
                  (loop [j 0 f nil]
                    (if (>= j MSGS-EACH)
                      f
                      (recur (+ j 1) (send a inc))))]
              (recur (rest as) (cons this-last fs)))))]
    (drain last-fs)
    :done))

(run)
(println (actor-stats))
