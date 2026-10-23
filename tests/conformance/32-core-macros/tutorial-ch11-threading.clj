;; EXPECT: (reduce + (map inc (filter even? [1 2 3 4]))) 8
;; docs/tutorial/11-macros.md §11.4.3: ->> is a macro.
(println (macroexpand '(->> [1 2 3 4] (filter even?) (map inc) (reduce +)))
         (->> [1 2 3 4] (filter even?) (map inc) (reduce +)))
