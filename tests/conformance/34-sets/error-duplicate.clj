;; EXPECT-ERROR: Duplicate key: :a
;; A set literal that repeats an element is a read error, as in Clojure.
(println #{:a :b :a})
