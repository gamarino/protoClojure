;; EXPECT: nil true
;; A global bound to nil resolves to nil; it is not an unresolved symbol.
(def x nil)
(println x (nil? x))
