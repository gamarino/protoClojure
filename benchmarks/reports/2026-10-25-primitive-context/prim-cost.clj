;; The cost of one primitive call, by primitive (2026-10-25).
;; args: case n mode   (mode = work | setup)
;; prints: RESULT case n mode checksum ns-per-iteration
;; `setup` runs the same loop without the call, so work - setup is the
;; call's own cost. Every loop's checksum is printed and checked by run.sh.
(def args *command-line-args*)
(def cs (nth args 0))
(def n (read-string (nth args 1)))
(def work? (= "work" (nth args 2)))

(def m {:a 1 :b 2 :c 3})
(def v [1 2 3])

(defn l-str [n] (loop [i 0 acc 0] (if (< i n) (recur (inc i) (+ acc (count (str "k" i)))) acc)))
(defn c-str [n] (loop [i 0 acc 0] (if (< i n) (recur (inc i) (+ acc 2)) acc)))
(defn l-assoc [n] (loop [i 0 acc 0] (if (< i n) (recur (inc i) (+ acc (count (assoc m :d i)))) acc)))
(defn c-assoc [n] (loop [i 0 acc 0] (if (< i n) (recur (inc i) (+ acc (count m))) acc)))
(defn l-get [n] (loop [i 0 acc 0] (if (< i n) (recur (inc i) (+ acc (get m :b))) acc)))
(defn c-get [n] (loop [i 0 acc 0] (if (< i n) (recur (inc i) (+ acc 2)) acc)))
(defn l-max [n] (loop [i 0 acc 0] (if (< i n) (recur (inc i) (+ acc (max i 7))) acc)))
(defn c-max [n] (loop [i 0 acc 0] (if (< i n) (recur (inc i) (+ acc i)) acc)))
(defn l-first [n] (loop [i 0 acc 0] (if (< i n) (recur (inc i) (+ acc (first v))) acc)))
(defn c-first [n] (loop [i 0 acc 0] (if (< i n) (recur (inc i) (+ acc 1)) acc)))
(defn l-reduce [n] (reduce (fn [acc x] (+ acc (max x 1))) 0 (range n)))
(defn c-reduce [n] (reduce (fn [acc x] (+ acc x)) 0 (range n)))

(def cases {"str" [l-str c-str] "assoc" [l-assoc c-assoc] "get" [l-get c-get]
            "max" [l-max c-max] "first" [l-first c-first] "reduce-max" [l-reduce c-reduce]})
(def f ((get cases cs) (if work? 0 1)))
(def t0 (System/nanoTime))
(def result (f n))
(def t1 (System/nanoTime))
(println "RESULT" cs n (if work? "work" "setup") result (/ (* 1.0 (- t1 t0)) n))
