;; EXPECT: false true false false false true true
;; A keyword is never = to the string of its spelling: a short spelling
;; (which protoCore stores inline, exactly like the string) and a long one,
;; through the EQ opcode, the variadic = primitive and apply.
(println (= :a ":a")
         (not= :a ":a")
         (= ":a" :a)
         (= :a-much-longer-keyword ":a-much-longer-keyword")
         (= :a :a ":a")
         (= :a :a)
         (apply not= (list :a ":a")))
