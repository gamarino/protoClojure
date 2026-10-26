;; EXPECT: 6888891 true true true :found 1 true
;; A list of 1,000,000 elements is printed, compared and hashed iteratively:
;; no recursion per element overflows the stack (docs/DESIGN.md, M44, M47).
(def a (into () (range 1000000)))
(def b (into () (range 1000000)))
(println (count (pr-str a)) (= a b) (= (hash a) (hash b)) (= (hash a) (hash (vec a)))
         (get {a :found} b) (count (conj (set [a]) b)) (= a (seq (vec b))))
