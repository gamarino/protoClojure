;; EXPECT: ok
;; The VM answers inc, dec, +, first, rest, next, seq, empty?, count and nth
;; on lists, vectors, cons cells and chunks without a call context
;; (fastPrimitiveCall); every answer must equal the primitive's, edge cases
;; included, and a redefined name must reach the redefinition.
(def v [1 2 3])
(def l '(1 2 3))
(def c (cons 0 (lazy-seq (list 1 2))))
(def ch (rest [9 8 7]))
(def checks
  [(= nil (first nil)) (= nil (first [])) (= nil (first ())) (= 1 (first v)) (= 1 (first l))
   (= 0 (first c)) (= 8 (first ch))
   (= () (rest nil)) (= () (rest [])) (= () (rest [1])) (= [2 3] (rest v)) (= '(2 3) (rest l))
   (= '(1 2) (rest c)) (= '(7) (rest ch)) (= () (rest (rest ch)))
   (= nil (next nil)) (= nil (next [1])) (= nil (next '(1))) (= [2 3] (next v)) (= '(2 3) (next l))
   (= '(1 2) (next c)) (= nil (next (rest ch)))
   (= nil (seq nil)) (= nil (seq [])) (= nil (seq ())) (= [1 2 3] (seq v)) (= c (seq c))
   (true? (empty? nil)) (true? (empty? [])) (true? (empty? ())) (false? (empty? v))
   (false? (empty? c)) (false? (empty? ch)) (true? (empty? (rest [1])))
   (= 0 (count nil)) (= 3 (count v)) (= 3 (count l)) (= 3 (count c)) (= 2 (count ch))
   (= 2 (nth v 1)) (= 3 (nth l 2)) (= :nf (nth v 5 :nf)) (= 1 (nth c 1)) (= 7 (nth ch 1))
   (= 2 (inc 1)) (= 0 (dec 1)) (= 1.5 (inc 0.5)) (= 9007199254740992 (inc 9007199254740991))
   (= -9007199254740993 (dec -9007199254740992)) (= 3 (+ 1 2)) (= 9007199254740992 (+ 9007199254740991 1))
   (= 6 (reduce + v)) (= 6 (reduce + 0 l))
   (= "IndexOutOfBoundsException"
      (try (nth v 3) (catch IndexOutOfBoundsException e "IndexOutOfBoundsException")))])
(def failed (filter (fn [i] (not (nth checks i))) (range (count checks))))
(def all-ok (empty? failed))
;; Last: core functions and macros written in Clojure call `first` too.
(def first (fn [x] :redefined))
(println (if (not all-ok) (pr-str failed)
           (if (not= :redefined (first v)) "redefinition ignored" "ok")))
