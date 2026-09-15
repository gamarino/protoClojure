;; EXPECT: 1 nil
;; Redefining a global to nil keeps it defined.
(def x 1)
(def before x)
(def x nil)
(println before x)
