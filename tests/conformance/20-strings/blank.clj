;; EXPECT: true true false false
(println (blank? "")
         (blank? "   \t\n")
         (blank? "x")
         (blank? "  x  "))
