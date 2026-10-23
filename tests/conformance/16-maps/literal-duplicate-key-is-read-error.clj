;; EXPECT-ERROR: Duplicate key: :a
;; As in Clojure, a map literal that repeats a key is a read error.
(println {:a 1 :b 2 :a 3})
