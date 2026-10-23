;; EXPECT: nil nil (1 2) (a b) ([:k 1]) (1 2 3 4) () true
;; seq answers nil for an empty collection; concat joins sequences.
(println (seq []) (seq nil) (seq [1 2]) (seq "ab") (seq {:k 1})
         (concat [1] '(2) nil [3 4]) (concat) (list? (seq '(1))))
