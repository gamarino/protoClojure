;; EXPECT: 3 ["out\n" "err\n"]
;; A non-zero exit is answered, not thrown; stderr is collected apart.
(def r (sh "sh" "-c" "echo out; echo err >&2; exit 3"))
(println (:exit r) (str [(:out r) (:err r)]))
