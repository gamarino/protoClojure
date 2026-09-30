;; EXPECT-ERROR: FileNotFoundException
;; An uncaught I/O error stops the script and names its class.
(slurp "/nonexistent-dir-protoclj/missing.txt")
