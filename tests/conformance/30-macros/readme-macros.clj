;; EXPECT: (if (empty? xs) nil (do (println "has" (count xs) "items")))
;; README.md, "A first macro": `unless` runs its body only when the test is
;; false; macroexpand-1 shows the code the macro wrote.
(defmacro unless [test & body]
  `(if ~test nil (do ~@body)))

(def xs [1 2 3])
(unless (empty? xs) (println "has" (count xs) "items"))   ;; => has 3 items
(println (pr-str (macroexpand-1 '(unless (empty? xs) (println "has" (count xs) "items")))))
