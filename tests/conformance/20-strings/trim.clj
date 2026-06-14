;; EXPECT: hello hello hello
(println (trim "  hello  ")
         (triml "  hello")
         (trimr "hello  "))
