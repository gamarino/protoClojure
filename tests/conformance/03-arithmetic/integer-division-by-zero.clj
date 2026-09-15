;; EXPECT-ERROR: ArithmeticException: Divide by zero
;; Integer division by zero raises the ArithmeticException analogue with JVM
;; Clojure's message.
(println (/ 1 0))
