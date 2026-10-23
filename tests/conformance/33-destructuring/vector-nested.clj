;; EXPECT: 1 2 3 4 5
;; Nested vector and map binding forms.
(let [[[a b] {[c d] :pt} [_ [e]]] [[1 2] {:pt [3 4]} [0 [5]]]]
  (println a b c d e))
