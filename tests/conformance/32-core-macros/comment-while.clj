;; EXPECT: nil 3
;; comment ignores its body; while loops while the test is true.
(def n (atom 0))
(while (< @n 3) (swap! n inc))
(println (comment (this is never run)) @n)
