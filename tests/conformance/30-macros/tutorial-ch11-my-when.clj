;; EXPECT: (if (= 1 1) (do (println "hi")))
;; docs/tutorial/11-macros.md §11.2: the smallest macro and macroexpand-1.
(defmacro my-when [test & body]
  (list 'if test (cons 'do body)))

;; Use:
(my-when (= 1 1)
  (println "yes")
  (println "yes again"))

(println (pr-str (macroexpand-1 '(my-when (= 1 1) (println "hi")))))
