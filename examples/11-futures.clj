;; Futures — `(future body)` spawns a real OS thread that evaluates
;; `body` independently of the caller. `@future` (or `(deref future)`)
;; blocks until the worker thread finishes and returns the value.

(defn fib [n] (if (<= n 1) n (+ (fib (- n 1)) (fib (- n 2)))))

;; A single future runs in parallel with the main thread.
(def f (future (fib 25)))
(println "main thread can keep doing things...")
(println "fib(25) =" @f)

;; Multiple futures: each runs on its own OS thread, all sharing the
;; same protoCore heap (no marshalling, no copy). Wall-clock scales
;; with the number of cores you have.
(def f1 (future (fib 25)))
(def f2 (future (fib 25)))
(def f3 (future (fib 25)))
(def f4 (future (fib 25)))
(println "four fib(25)s in parallel:" @f1 @f2 @f3 @f4)

;; Manual pmap pattern — until session-N's parallel pmap lands.
(defn sq [x] (* x x))
(def squares-futures
  (map (fn [x] (future (sq x))) (list 10 20 30 40 50)))
(println "squares:" (map deref squares-futures))

;; future? / realized? predicates.
(println (future? f1)        ; true
         (future? 42)        ; false
         (realized? f1))     ; true once we've @-ed it above

;; Futures + atoms — concurrent writers, all CAS-safe.
(def counter (atom 0))
(def workers
  (map (fn [_] (future (swap! counter inc)))
       (list 1 2 3 4 5 6 7 8 9 10)))
;; Force all workers to finish — `(map deref workers)` is lazy in
;; Clojure-JVM but eager in v0.x, so just walking it forces realisation.
(println "results:" (map deref workers))
(println "counter after 10 concurrent swaps:" @counter)
