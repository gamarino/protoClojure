;; EXPECT-ERROR: ClassCastException: re-find expects a pattern, got a string
;; re-find takes a pattern, as in Clojure.
(re-find "a" "abc")
