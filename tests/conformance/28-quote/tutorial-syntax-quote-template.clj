;; EXPECT: (if (> x 0) (do (println pos) :pos) nil)
;; docs/tutorial/11-macros.md §11.3: a syntax-quote template with ~ and ~@.
(def test-form '(> x 0))
(def body '((println "pos") :pos))
(println `(if ~test-form (do ~@body) nil))
