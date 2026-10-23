;; EXPECT: :b nil
;; A recursive macro: `my-cond` expands into an `if` whose else branch is
;; another `my-cond` call, which the compiler expands in turn.
(defmacro my-cond [& clauses]
  (when (seq clauses)
    (list 'if (first clauses)
          (second clauses)
          (cons 'my-cond (next (next clauses))))))
(defn classify [n] (my-cond (< n 0) :a (= n 0) :b))
(println (classify 0) (classify 5))
