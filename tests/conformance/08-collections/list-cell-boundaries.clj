;; EXPECT: ok
;; A list is a protoCore ProtoLinkedList: cells of four elements, every cell
;; full but the head (docs/DESIGN.md, M46). Every size from 0 to 13, built
;; five ways, is read every way against the vector of the same elements, so
;; each operation crosses a partial head, a full head and a cell boundary.
(defn walk-rest [l] (loop [l l acc []] (if (empty? l) acc (recur (rest l) (conj acc (first l))))))
(defn walk-next [l] (loop [s (seq l) acc []] (if s (recur (next s) (conj acc (first s))) acc)))
(defn builds [n]
  (let [v (vec (range n))]
    [(apply list v)
     (into () (reverse v))
     (reduce (fn [acc x] (conj acc x)) () (reverse v))
     (rest (apply list -1 v))
     (if (pos? n) (conj (apply list (rest v)) (first v)) ())]))
(defn check [n l]
  (let [v (vec (range n))]
    (and (list? l) (= n (count l)) (= l v) (= v l) (= (hash l) (hash v))
         (= v (walk-rest l)) (= v (walk-next l)) (= v (vec l)) (= v (into [] l))
         (every? (fn [i] (= i (nth l i))) (range n)) (= :nf (nth l n :nf))
         (= (reduce + 0 v) (reduce + 0 l)) (= (reduce + v) (reduce + l))
         (= (map inc v) (map inc l)) (= (seq (drop 3 v)) (seq (drop 3 l)))
         (= (last v) (last l)) (= (take 6 v) (take 6 l))
         (= :k (get {v :k} l)) (contains? #{v} l)
         (= (pr-str (seq v)) (if (zero? n) "nil" (pr-str l)))
         (or (zero? n) (and (= 0 (peek l)) (= (rest v) (pop l))))
         (= (cons :x v) (cons :x l)) (= (concat v v) (concat l l))
         (= (reverse v) (reverse l)) (= v (sort (reverse l)))
         (= (apply + v) (apply + l)) (= (str v) (str (vec l))))))
(def failed
  (for [n (range 14) k (range 5) :when (not (check n (nth (builds n) k)))] [n k]))
(def nils (list nil 1 nil 2 nil))
(def extra
  [(= [nil 1 nil 2 nil] (walk-rest nils)) (= 5 (count nils)) (nil? (nth nils 4))
   (= '(1 nil 2 nil) (rest nils)) (= "(nil 1 nil 2 nil)" (pr-str nils))
   (= (conj nils :a) (list :a nil 1 nil 2 nil))])
(println (if (and (empty? failed) (every? true? extra)) "ok" (pr-str failed extra)))
