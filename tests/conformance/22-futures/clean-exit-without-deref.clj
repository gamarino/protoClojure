;; EXPECT: spawned 50 futures
;; Regression test for the future-worker exit segfault: spawn many
;; futures, never @-them, and require a clean (exit 0) shutdown. Before
;; the FutureRegistry / shutdownFutures fix, this script crashed at
;; exit 10/10 with SIGSEGV (worker threads dereferencing freed Cells
;; while ProtoSpace destructed).

(defn fib [n] (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))

(defn launch [k]
  (loop [i 0 acc (list)]
    (if (>= i k) acc
        (recur (+ i 1) (cons (future (fib 18)) acc)))))

(def _ (launch 50))
(println "spawned" 50 "futures")
