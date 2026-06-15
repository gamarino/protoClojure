;; MPMC — Multi-Producer Multi-Consumer.
;;
;; N producer threads (futures) × M actor consumers. Each producer
;; round-robins K sends across the M actors (so each actor receives
;; N×K/M messages from the M senders). The worker pool drains the M
;; actors in parallel (different actors can run on different workers
;; simultaneously; the single-method invariant only forbids the same
;; actor running twice). Saturates BOTH the per-actor mailbox path
;; AND the global ready queue.
;;
;; Total messages = N × K. With N=M=4 producers/actors, each actor
;; sees K msgs from each producer = N×K/M = K msgs/actor on average.

(def PRODUCERS 4)
(def CONSUMERS 4)
(def PER-PRODUCER 250000)
(def TOTAL (* PRODUCERS PER-PRODUCER))

(defn drain [futures]
  (loop [fs futures]
    (if (empty? fs)
      :done
      (do @(first fs) (recur (rest fs))))))

(defn make-actors [n]
  (loop [i 0 acc (list)]
    (if (>= i n) acc
        (recur (+ i 1) (cons (actor 0) acc)))))

(defn nth-actor [actors idx]
  (loop [as actors i idx]
    (if (= i 0) (first as) (recur (rest as) (- i 1)))))

(defn launch [actors]
  (loop [p 0 fs (list)]
    (if (>= p PRODUCERS)
      fs
      (recur (+ p 1)
             (cons (future
                     ;; Round-robin across actors via an explicit wrap
                     ;; counter (no `rem` primitive in v0.1.x).
                     (loop [j 0 idx 0 last-f nil]
                       (if (>= j PER-PRODUCER)
                         last-f
                         (let [target (nth-actor actors idx)
                               nxt    (if (>= (+ idx 1) CONSUMERS)
                                        0
                                        (+ idx 1))]
                           (recur (+ j 1) nxt (send target inc))))))
                   fs)))))

(defn final-drain [actors]
  ;; Send one synchronous probe per actor so we know all prior messages
  ;; have been processed (single-method invariant on each actor).
  (loop [as actors]
    (when-not (empty? as)
      @(send (first as) (fn [v] v))
      (recur (rest as)))))

(defn run []
  (let [actors    (make-actors CONSUMERS)
        producers (launch actors)]
    (drain producers)
    (final-drain actors)
    :done))

(run)
(println (actor-stats))
