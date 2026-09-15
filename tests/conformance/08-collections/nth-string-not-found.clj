;; EXPECT: :none :none b
;; With a not-found value, an index out of range of a string returns it.
(println (nth "abc" 3 :none) (nth "abc" -1 :none) (nth "abc" 1 :none))
