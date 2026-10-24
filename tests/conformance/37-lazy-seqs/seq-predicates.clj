;; EXPECT: true true false true false true true false
;; seq? is true for every seq (lists, lazy seqs, cons cells, a vector's
;; seq), not for a vector; list? only for lists; chunked-seq? for the seq of
;; a vector.
(println (seq? (map inc [1])) (seq? '(1)) (seq? [1]) (seq? (cons 1 (lazy-seq nil)))
         (list? (map inc [1])) (seq? (seq [1 2])) (chunked-seq? (seq [1 2]))
         (chunked-seq? (seq '(1 2))))
