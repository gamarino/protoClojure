;; EXPECT: a1 a2 b1 b2 nil 3
;; doseq runs its body for each element, nested left to right, and answers
;; nil; it walks lists, vectors and maps.
(def out (atom []))
(def r (doseq [x ["a" "b"] y [1 2]] (swap! out conj (str x y))))
(def n (atom 0))
(doseq [e {:a 1 :b 2 :c 3}] (swap! n inc))
(println (nth @out 0) (nth @out 1) (nth @out 2) (nth @out 3) r @n)
