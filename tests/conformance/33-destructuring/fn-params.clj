;; EXPECT: 6 [1 nil nil] [1 2 3] 10
;; Destructuring in fn and defn parameters, including the variadic rest.
(defn f [[a b] {:keys [c]}] (+ a b c))
(defn g [x & [y z]] [x y z])
(println (f [1 2] {:c 3}) (g 1) (g 1 2 3) ((fn [{[p q] :pair}] (* p q)) {:pair [2 5]}))
