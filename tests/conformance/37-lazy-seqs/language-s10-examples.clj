;; EXPECT: (1 2 3) true false (2 3 4 5) (0 1 2 3 4) (1 9 25 49 81) (0 1 1 2 3 5 8 13 21 34) (1 2 0 1 2) 32 false
;; The examples of LANGUAGE.md §10, as written there.
(def a (seq [1 2 3]))
(def b (seq? (map inc [1])))
(def c (list? (map inc [1])))
(def d (rest (map inc (range 5))))
(def e (take 5 (iterate inc 0)))
(def odd-squares
  (->> (range)
       (filter odd?)
       (map (fn [x] (* x x)))))
(defn fib-seq
  ([] (fib-seq 0 1))
  ([a b] (lazy-seq (cons a (fib-seq b (+ a b))))))
(def calls (atom 0))
(first (map (fn [x] (swap! calls inc) x) (range 100)))
(doseq [x [1 2 3]] (println x))
(dorun (map println [1 2]))
(println a b c d e (take 5 odd-squares) (take 10 (fib-seq)) (take 5 (lazy-cat [1 2] (range))) @calls
         (realized? (lazy-seq [1])))
