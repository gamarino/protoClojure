;; EXPECT: true true true nil true
;; conj adds, disj removes (several at once); an element already present or
;; absent leaves the set equal; disj of nil is nil.
(def s #{1 2})
(println (= (conj s 3 4) #{1 2 3 4})
         (= (disj s 1) #{2})
         (= (disj (conj s 2) 5 6) s)
         (disj nil 1)
         (= (disj #{1 2 3} 1 2) #{3}))
