;; EXPECT: (0 1 4 9 16)
;; Tutorial chapter 5, §5.9: map over an endless range, with #(...).
(def squares (map #(* % %) (range)))   ;; squares of all naturals — lazy

(println (take 5 squares))             ;; => (0 1 4 9 16)
