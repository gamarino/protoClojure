;; Maximum messages/sec throughput.
;;
;; Mode is selected by the env var `PROTOCLJ_BENCH_MODE`:
;;
;;   single  — one actor, 100_000 inc messages. Stresses the actor's
;;             own mailbox lock — more workers DON'T help here
;;             because of the single-method invariant.
;;
;;   fan-out — 1_000 actors, 100 inc messages each (100_000 total).
;;             Stresses the ready-queue scheduler — more workers
;;             help up to the number of CPU cores.
;;
;; Default mode is "single". The companion `actor-bench.sh` runs both
;; modes across worker counts.

;; Mode chosen by which top-level call is in effect at the bottom of
;; the file. The companion `actor-bench.sh` runs each .clj file
;; separately, so we don't need a runtime switch.
(def MODE :single)

(defn drain [futures]
  (loop [fs futures]
    (if (empty? fs)
      :done
      (do @(first fs)
          (recur (rest fs))))))

(defn single-mode []
  (let [N 1000000
        a (actor 0)
        last-f
        (loop [i 0 f nil]
          (if (>= i N)
            f
            (recur (+ i 1) (send a inc))))]
    @last-f
    @a))

(defn fan-out-mode []
  (let [ACTORS 1000
        MSGS-EACH 100
        actors
        (loop [i 0 acc (list)]
          (if (>= i ACTORS)
            acc
            (recur (+ i 1) (cons (actor 0) acc))))
        ;; Spray MSGS-EACH messages at each actor.
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

(if (= MODE :fan-out)
  (fan-out-mode)
  (single-mode))

(println (actor-stats))
