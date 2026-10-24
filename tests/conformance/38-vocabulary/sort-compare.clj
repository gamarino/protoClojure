;; EXPECT: (1 2 3) (3 2 1) (a bb ccc) ({:n 3} {:n 1}) ([1 :b] [1 :a] [2 :c]) (nil :a :b) -1 1 0 -1 ("a" "b" "c")
;; sort with compare (or a comparator fn, or a boolean predicate as Clojure
;; allows), stable; sort-by with a key fn; compare.
(println (sort [3 1 2]) (sort > [3 1 2]) (sort-by count ["ccc" "a" "bb"])
         (sort-by :n #(compare %2 %1) [{:n 1} {:n 3}])
         (sort-by first [[1 :b] [2 :c] [1 :a]]) (sort [:b nil :a])
         (compare "a" "b") (compare [1 3] [1 2]) (compare 1 1.0) (compare nil 0)
         (pr-str (sort (set ["c" "a" "b"]))))
