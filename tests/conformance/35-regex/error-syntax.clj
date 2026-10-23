;; EXPECT-ERROR: PatternSyntaxException
;; A literal the engine rejects is a read error.
(println #"(ab")
