;; EXPECT: 400 true
;; Concurrent reset! calls on one atom from four threads: every watch call
;; receives as its old value the exact value its own reset! replaced, so each
;; of the 400 replaced values (0 and every value but the last) is reported as
;; old exactly once.
(def a (atom 0))
(def olds (atom {}))
(add-watch a :log (fn [k r o n] (swap! olds (fn [m] (assoc m o (inc (get m o 0)))))))
(defn resets [t i]
  (if (= i 100) :done (do (reset! a (+ (* t 1000) i 1)) (recur t (inc i)))))
(pmap (fn [t] (resets t 0)) [1 2 3 4])
(def m @olds)
(println (count m) (= (reduce + (vals m)) (count m)))
