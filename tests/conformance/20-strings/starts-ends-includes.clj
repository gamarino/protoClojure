;; EXPECT: true true true false
(println (starts-with? "hello world" "hello")
         (ends-with?   "hello world" "world")
         (includes?    "hello world" "lo wo")
         (includes?    "hello"       "xyz"))
