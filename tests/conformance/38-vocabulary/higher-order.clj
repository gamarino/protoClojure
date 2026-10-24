;; EXPECT: 3 3 [6 4] true 7 3 2 0 true true true true false
;; partial, comp, juxt, complement, constantly, identity, fnil, some, every?,
;; not-any?, not-every?.
(println ((partial + 1) 2) ((comp inc inc) 1) ((juxt inc dec) 5) ((complement odd?) 2)
         ((constantly 7) 1 2) (identity 3) ((fnil inc 1) nil) ((fnil inc 1) -1)
         (some even? [1 3 4]) (every? odd? [1 3]) (not-any? even? [1 3]) (not-every? odd? [1 2])
         (every? odd? [1 2]))
