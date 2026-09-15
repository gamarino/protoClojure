;; EXPECT: true true true true false false
;; Sequential equality ignores the concrete type: a vector and a list with
;; equal elements in the same order are =, through the EQ opcode, the
;; variadic primitive and = as a function value.
(def eq =)
(println (= [1 2] (list 1 2))
         (= (list 1 2) [1 2])
         (= [1 2] (list 1 2) [1 2])
         (eq (list :a "b") [:a "b"])
         (= [1 2] (list 2 1))
         (not= [1 2] (list 1 2)))
