;; args: case n mode   (mode = work | setup)
;; prints: case n mode checksum ns-per-element
(def args *command-line-args*)
(def cs (read-string (nth args 0)))
(def n (read-string (nth args 1)))
(def work? (= "work" (nth args 2)))

(defn mk-list [] (apply list (range n)))
(defn mk-vec [] (vec (range n)))

(defn walk [xs] (loop [xs xs acc 0] (if (empty? xs) acc (recur (rest xs) (+ acc (first xs))))))
(defn build [n] (count (loop [i 0 acc '()] (if (< i n) (recur (inc i) (cons i acc)) acc))))
(defn seqnth [v n] (loop [i 0 acc 0] (if (< i n) (recur (inc i) (+ acc (nth v i))) acc)))
(defn seqctl [v n] (loop [i 0 acc 0] (if (< i n) (recur (inc i) (+ acc i)) acc)))
(defn rndnth [v n] (loop [i 0 x 12345 acc 0]
                   (if (< i n)
                     (recur (inc i) (mod (+ (* 1664525 x) 1013904223) 4294967296) (+ acc (nth v (mod x n))))
                     acc)))
(defn rndctl [v n] (loop [i 0 x 12345 acc 0]
                   (if (< i n)
                     (recur (inc i) (mod (+ (* 1664525 x) 1013904223) 4294967296) (+ acc (mod x n)))
                     acc)))

(defn probe-ef [xs n] (loop [i 0 acc 0] (if (< i n) (recur (inc i) (+ acc (if (empty? xs) 1 (first xs)))) acc)))
(defn probe-rest [xs n] (loop [i 0 acc 0] (if (< i n) (recur (inc i) (+ acc (count (rest xs)))) acc)))
(defn probe-cons [xs n] (loop [i 0 acc 0] (if (< i n) (recur (inc i) (+ acc (count (cons i xs)))) acc)))

(def data (cond (= cs 7) (mk-list) (= cs 8) (mk-list) (= cs 9) (mk-list) (= cs 1) (mk-list) (= cs 3) (mk-list) (= cs 4) nil :else (mk-vec)))
;; force realisation of setup data before timing
(def setup-check (if data (count data) 0))

(def t0 (System/nanoTime))
(def result
  (if work?
    (cond (= cs 1) (walk data)
          (= cs 2) (walk (seq data))
          (= cs 3) (reduce + 0 data)
          (= cs 4) (build n)
          (= cs 5) (seqnth data n)
          (= cs 50) (seqctl data n)
          (= cs 6) (rndnth data n)
          (= cs 60) (rndctl data n)
          (= cs 7) (probe-ef data n)
          (= cs 8) (probe-rest data n)
          (= cs 9) (probe-cons data n))
    -1))
(def t1 (System/nanoTime))
(println "RESULT" cs n (nth args 2) setup-check result (- t1 t0))
