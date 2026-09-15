;; EXPECT: false :falsy
(def x false)
(println x (if x :truthy :falsy))
