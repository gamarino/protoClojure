;; EXPECT: (1 2 a b) (0 1 2 3) (1 1 2 2 3 3) (0 :a 1 :b) (1 2 0 1 2)
;; concat of collections and of lazy seqs, mapcat, lazy-cat.
(println (concat [1] '(2) nil "ab") (concat (range 2) (lazy-seq [2 3]))
         (mapcat (fn [x] [x x]) [1 2 3]) (mapcat (fn [a b] [a b]) (range) [:a :b])
         (take 5 (lazy-cat [1 2] (range))))
