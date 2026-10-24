;; EXPECT-ERROR: Nested #()s are not allowed
;; As in Clojure, a #(...) cannot contain another.
(#(map #(inc %) %) [1])
