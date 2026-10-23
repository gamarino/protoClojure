;; EXPECT: :caught 3
;; A try / catch written in a template: the class name stays a class name
;; and the binding is an auto-gensym.
(defmacro safely [& body] `(try ~@body (catch Exception e# :caught)))
(println (safely (/ 1 0)) (safely (+ 1 2)))
