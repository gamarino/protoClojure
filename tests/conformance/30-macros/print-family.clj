;; EXPECT: a b "c" :d "e" :f
;; print and pr write without a newline: print as println does, pr readably,
;; as prn does.
(print "a" "b ")
(pr "c" :d)
(print " ")
(prn "e" :f)
