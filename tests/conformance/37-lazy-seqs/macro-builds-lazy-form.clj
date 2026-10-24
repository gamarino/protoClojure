;; EXPECT: 2
;; A macro may build its expansion with map: the lazy seq it answers is a
;; list form to the compiler.
(defmacro print-each [& xs] (cons 'do (map (fn [x] (list 'println x)) xs)))
(print-each 1 2)
