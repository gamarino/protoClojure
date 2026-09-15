;; EXPECT: true nil true nil 1 #<promise nil> #<promise pending>
;; A promise delivered nil is realized with the value nil; a second deliver
;; changes nothing and returns nil; a pending promise prints as pending.
(def p (promise))
(def q (promise))
(println (promise? (deliver p nil))
         @p
         (realized? p)
         (deliver p 1)
         (do (deliver q 1) @q)
         p
         (promise))
