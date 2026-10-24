;; EXPECT: 32 32 1
;; As in Clojure, map over a vector or a range realises 32 elements at a
;; time (chunked seqs); over a list, one at a time.
(def n (atom 0))
(defn count-calls [coll] (reset! n 0) (first (map (fn [x] (swap! n inc) x) coll)) @n)
(println (count-calls (vec (range 100))) (count-calls (range 100)) (count-calls '(1 2 3)))
