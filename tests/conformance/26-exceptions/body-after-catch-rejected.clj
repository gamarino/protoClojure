;; EXPECT-ERROR: Only catch or finally clause can follow catch in try expression
(try 1 (catch Exception e 2) 3)
