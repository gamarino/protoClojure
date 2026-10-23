;; EXPECT: :b nil (if a 1 (clojure.core/cond b 2))
;; cond is a macro from core.clj now: macroexpand shows its expansion.
(defn g [n] (cond (< n 0) :a (= n 0) :b :else :c))
(println (g 0) (cond) (macroexpand-1 '(cond a 1 b 2)))
