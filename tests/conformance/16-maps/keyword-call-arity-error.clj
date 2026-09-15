;; EXPECT-ERROR: keyword :a called as a function expects 1 or 2 arguments, got 3
;; A keyword takes a map and an optional not-found value, nothing more.
(:a {:a 1} 2 3)
