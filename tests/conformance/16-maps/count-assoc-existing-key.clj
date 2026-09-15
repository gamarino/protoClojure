;; EXPECT: 2 9 8 2
;; assoc of keys equal to existing ones (a string built at run time, a list
;; equal to a vector key) replaces the values and adds no entry.
(def m (assoc {"key-number-one" 1 [1 2] 2} (str "key-number-" "one") 9 (list 1 2) 8))
(println (count m) (get m "key-number-one") (get m [1 2]) (count (keys m)))
