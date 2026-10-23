;; EXPECT: (if (> x 0) (do (println "pos") :pos) nil) hi! (clojure.core/str "hi" "!") (do (println "hi") (println "hi"))
;; docs/tutorial/11-macros.md §11.3 and §11.3.1: templates, qualified
;; built-ins that a caller's local does not capture, special forms unchanged.
(defmacro my-when [test & body]
  `(if ~test
     (do ~@body)))
(my-when true (print ""))
(def test-form '(> x 0))
(def body '((println "pos") :pos))
(defmacro shout [x] `(str ~x "!"))
(defmacro doublet [body]
  `(do ~body ~body))
(println (pr-str `(if ~test-form (do ~@body) nil))
         (let [str 0] (shout "hi"))
         (pr-str (macroexpand-1 '(shout "hi")))
         (pr-str (macroexpand-1 '(doublet (println "hi")))))
