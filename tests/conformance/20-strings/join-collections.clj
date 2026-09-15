;; EXPECT: {:a 1}, #<atom 2>, [3]
;; join renders each element the way println prints it, maps and atoms
;; included.
(println (join ", " [{:a 1} (atom 2) [3]]))
