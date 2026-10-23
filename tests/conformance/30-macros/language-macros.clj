;; EXPECT: (if done? nil (do (stop))) 42
;; LANGUAGE.md §6.1: defmacro, macroexpand-1 and eval.
(defmacro unless [test & body]
  `(if ~test nil (do ~@body)))

(unless false (println "runs"))
(println (macroexpand-1 '(unless done? (stop))) (eval (list 'unless false 42)))
