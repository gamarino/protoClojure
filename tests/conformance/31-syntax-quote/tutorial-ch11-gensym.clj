;; EXPECT: 9 clojure.core/let true
;; docs/tutorial/11-macros.md §11.3.2: x# is a fresh symbol, the same
;; throughout the template.
(defmacro square-of [expr]
  `(let [x# ~expr]
     (* x# x#)))
(def e (macroexpand-1 '(square-of (+ 1 2))))
(println (square-of (+ 1 2)) (first e)
         (= (first (second e)) (second (nth e 2)) (nth (nth e 2) 2)))
