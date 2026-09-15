;; EXPECT-ERROR: unexpected character: → (U+2192)
;; Beyond ASCII only letters, combining marks and decimal digits extend a
;; symbol; any other character, such as an arrow, is a read error.
(def a→b 1)
