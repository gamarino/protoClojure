;; EXPECT-ERROR: finally clause must be last in try expression
(try 1 (finally 2) (catch Exception e 3))
