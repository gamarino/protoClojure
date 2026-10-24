;; EXPECT-ERROR: IndexOutOfBoundsException
;; A vector called with an index it does not have raises, as in Clojure.
([1 2] 5)
