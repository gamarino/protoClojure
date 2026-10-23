;; EXPECT: (1 2 3 4) nil (1) (a b)
;; list* prepends its leading arguments to the elements of its last one.
(println (list* 1 2 [3 4]) (list* nil) (list* 1 nil) (list* 'a '(b)))
