;; EXPECT: ran (if (pos? x) nil (do (println "no")))
;; The classic first macro: `unless` runs its body when the test is false.
;; The arguments arrive unevaluated, as data, and the macro builds the code.
(defmacro unless [test & body]
  (list 'if test nil (cons 'do body)))
(unless false (print "ran "))
(unless true (println "never"))
(println (pr-str (macroexpand-1 '(unless (pos? x) (println "no")))))
