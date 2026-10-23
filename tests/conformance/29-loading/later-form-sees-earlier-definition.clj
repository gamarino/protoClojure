;; EXPECT: 3 30
;; Each top-level form runs before the next is compiled, so redefining a
;; global between two forms changes what the second one calls.
(defn g [] 1)
(defn f [] (+ (g) 2))
(println (f) (do (defn g [] 28) (f)))
