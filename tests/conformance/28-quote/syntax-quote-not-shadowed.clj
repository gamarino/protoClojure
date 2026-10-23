;; EXPECT: (user/a 7 8)
;; The expansion names clojure.core/seq, clojure.core/concat and
;; clojure.core/list: a local of the same name does not capture them.
(defn f [list seq concat] `(a ~list ~seq))
(println (f 7 8 9))
