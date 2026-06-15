;; Three concurrency primitives layered together: watches on atoms,
;; promises for one-shot handoff, parallel pmap for fan-out work.

;; --- Watches ---------------------------------------------------------
(def counter (atom 0))
(def audit (atom (list)))

(add-watch counter :audit
  (fn [k a old new]
    (swap! audit (fn [l] (cons (list :tick old new) l)))))

(reset! counter 1)
(swap! counter inc)
(swap! counter inc)
(remove-watch counter :audit)
(reset! counter 99)                   ;; this one is NOT audited

(println "counter:" @counter)
(println "audit log (newest first):" @audit)

;; --- Promises --------------------------------------------------------
;; The classic "background fetch + main thread waits for it" pattern.
(def result (promise))

(def worker
  (future
    (deliver result (+ 100 200))))   ;; pretend this is slow work

(println "main thread does other stuff first...")
(println "result from background:" @result)
(println "realized?" (realized? result))

;; --- Parallel pmap --------------------------------------------------
;; Real OS-thread parallelism, transparent surface.
(defn fib [n] (if (<= n 1) n (+ (fib (- n 1)) (fib (- n 2)))))

(println "fibs in parallel:" (pmap fib (list 25 26 27 28)))
