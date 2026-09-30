;; EXPECT: 0 ["hi\n" ""]
;; sh answers {:exit :out :err}.
(def r (sh "echo" "hi"))
(println (:exit r) (str [(:out r) (:err r)]))
