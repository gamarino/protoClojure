;; MPSC — Multi-Producer Single-Consumer.
;;
;; N producer threads (futures) each fire K `(send a inc)` calls into
;; ONE shared actor. Total messages = N × K. This is the case where the
;; per-actor mailbox is contended by multiple sender threads — the
;; lock-free port's structural advantage.
;;
;; Wall-clock measures end-to-end: from start of first send to "actor
;; has processed every send the producers fired". The producers return
;; when their last send's promise resolves; we await all producer
;; futures; then we send one final probe message to make sure no extra
;; sends are still queued.
;;
;; Throughput cap: even with lock-free senders, the single actor can
;; only process one message at a time (single-method invariant). So
;; total wall-clock is bounded below by `K × N × per-msg-process-time`.
;; The interesting question is: does the sender-side contention show
;; up as a wall-clock cost on top of that?

(def PRODUCERS 4)
(def PER-PRODUCER 250000)
(def TOTAL (* PRODUCERS PER-PRODUCER))

(defn drain [futures]
  (loop [fs futures]
    (if (empty? fs)
      :done
      (do @(first fs) (recur (rest fs))))))

(defn launch [a]
  (loop [i 0 fs (list)]
    (if (>= i PRODUCERS)
      fs
      (recur (+ i 1)
             (cons (future
                     (loop [j 0 last-f nil]
                       (if (>= j PER-PRODUCER)
                         last-f
                         (recur (+ j 1) (send a inc)))))
                   fs)))))

(defn run []
  (let [a (actor 0)
        producers (launch a)]
    (drain producers)
    ;; A final synchronous probe so we know every prior send has been
    ;; processed (the single-method invariant guarantees serial
    ;; processing of mailbox entries, so this send only completes
    ;; after every previous send).
    @(send a (fn [v] v))
    @a))

(run)
(println (actor-stats))
