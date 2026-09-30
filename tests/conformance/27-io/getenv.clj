;; EXPECT: true true true true
;; getenv answers one variable (nil when unset) or every variable as a map.
(def all (getenv))
(println (string? (getenv "PATH"))
         (nil? (getenv "PROTOCLJ_SURELY_UNSET_VARIABLE"))
         (map? all)
         (= (get all "PATH") (getenv "PATH")))
