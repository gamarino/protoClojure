;; Saturation — 32 actors x 150 CPU-bound messages, one sender thread.
;;
;; The twin of protoScala's `benchmarks/actors/actor-saturation-32.scala`, so
;; that the actors-versus-workers curve can be read on the same machine with
;; the same shape. It also mirrors protoST's `benchmarks/actors/saturation_32a.st`.
;;
;; Why CPU-bound. The existing actor benchmarks here and in protoScala all
;; carry trivial handlers (`inc`), so their throughput is set by how fast one
;; sender thread can enqueue, not by how much work the pool can absorb. None of
;; them can therefore show a rise up to the machine's physical core count.
;; Every message here runs a 20,000-iteration summation instead — roughly
;; 1.5 ms of interpreter work — so the send loop costs well under 1% of the run
;; and the workers are the constraint. With 32 actors there are always more
;; runnable actors than workers, so the pool can steal freely. Total work is
;; identical to `actor-saturation-8.clj`, so the two curves are comparable.
;;
;; Self-reporting: `processed` is the sum the actors actually computed, not a
;; message count, so a handler that silently did no work cannot pass. Each
;; message adds sum(1..20000) = 200,010,000 to its actor's state, so the total
;; over the 32 actors is 4800 * 200,010,000. The closing probe returns the state
;; without changing it, and the single-method invariant orders it behind every
;; prior send.

(def ACTORS 32)
(def MSGS-EACH 150)
(def ITERS 20000)
(def PER-MSG 200010000)                 ; sum(1..ITERS)
(def TOTAL (* ACTORS MSGS-EACH))
(def EXPECTED (* TOTAL PER-MSG))

(defn work [s]
  (loop [i 1 sum 0]
    (if (> i ITERS)
      (+ s sum)
      (recur (+ i 1) (+ sum i)))))

(defn make-actors [n]
  (loop [i 0 acc (list)]
    (if (>= i n) acc
        (recur (+ i 1) (cons (actor 0) acc)))))

(defn send-round [actors]
  (loop [as actors]
    (when-not (empty? as)
      (send (first as) work)
      (recur (rest as)))))

(defn fire [actors]
  (loop [r 0]
    (when (< r MSGS-EACH)
      (send-round actors)
      (recur (+ r 1)))))

(defn collect [actors]
  ;; One synchronous probe per actor. The single-method invariant guarantees
  ;; every earlier message has been folded into the state it returns.
  (loop [as actors total 0]
    (if (empty? as)
      total
      (recur (rest as) (+ total @(send (first as) (fn [v] v)))))))

(defn run []
  (let [actors (make-actors ACTORS)]
    (fire actors)
    (collect actors)))

(def processed (run))
(println (str "mode=saturation-32 messages=" (+ TOTAL ACTORS)
              " processed=" processed))
(println (actor-stats))
(println (if (= processed EXPECTED) "ok" "FAILED"))
