;; EXPECT: (* 6 7) * (6 7) 3
;; docs/tutorial/11-macros.md §11.1: a quoted form is a list to take apart.
(def form '(* 6 7))
(println form (first form) (rest form) (count form))
