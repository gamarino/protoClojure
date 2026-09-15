;; EXPECT: :ñandú true false 1 ave :日本語 :Ωμέγα :ñandú!
;; Keywords spelled with non-ASCII letters read, print, intern and work as
;; map keys and as functions like ASCII ones.
(println :ñandú
         (= :ñandú :ñandú)
         (= :ñandú ":ñandú")
         (get {:ñandú 1} :ñandú)
         (:ñandú {:ñandú "ave"})
         :日本語
         :Ωμέγα
         (str :ñandú "!"))
