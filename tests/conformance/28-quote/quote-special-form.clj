;; EXPECT: (quote x) (if a b) [1 2]
;; (quote form) is what 'form reads as; quoting does not evaluate.
(println ''x (quote (if a b)) (quote [1 2]))
