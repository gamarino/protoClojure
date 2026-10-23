;; EXPECT: 10
;; A local named like a macro is called as the local, as in Clojure.
(defmacro twice [x] (list '* 2 x))
(defn f [twice] (twice 5))
(println (f (fn [n] (* n 2))))
