;; The garbage for the adaptive heap checks (adaptive_heap_check.py,
;; tests/CMakeLists.txt): each round builds a 50-element list of fresh strings
;; and drops it, and nothing is kept. The first command-line argument is the
;; number of rounds. The last line is the sentinel the check requires, with
;; the work done, so a run that stopped early cannot pass. Vectors are avoided:
;; they are protoCore tuples, interned and never collected.

(defn parse-nat [s]
  (loop [i 0 acc 0]
    (if (>= i (count s))
      acc
      (recur (+ i 1) (+ (* acc 10) (index-of "0123456789" (subs s i (+ i 1))))))))

(defn one-round [r]
  (loop [j 0 xs (list)]
    (if (>= j 50)
      (count xs)
      (recur (+ j 1) (cons (str "g-" r "-" j) xs)))))

(defn churn [rounds]
  (loop [r 0 made 0]
    (if (>= r rounds)
      made
      (recur (+ r 1) (+ made (one-round r))))))

(defn main [args]
  (let [rounds (if (nil? args) 20000 (parse-nat (first args)))]
    (println (str "adaptive heap garbage done: rounds=" rounds " objects=" (churn rounds)))))

(main *command-line-args*)
