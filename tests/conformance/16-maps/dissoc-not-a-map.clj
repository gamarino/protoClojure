;; EXPECT-ERROR: ClassCastException: dissoc expects a map, got a vector
;; dissoc takes a map or nil.
(dissoc [1 2] 0)
