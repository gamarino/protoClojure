;; EXPECT: [1 3 {:a 1} 4]
;; #_ discards the next form wherever a form may stand, nested #_ included.
#_(this is never read as code)
(println [1 #_2 3 {:a 1 #_ #_ :b 2} #_ #_ 9 8 4])
#_ :trailing
