;; EXPECT-ERROR: unpaired surrogate
;; A lone surrogate cannot be held by a UTF-8 string (D55).
(println "\uD83D")
