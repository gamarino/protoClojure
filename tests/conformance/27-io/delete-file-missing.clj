;; EXPECT: Couldn't delete /nonexistent-dir-protoclj/x
;; delete-file of nothing throws IOException unless told to be silent.
(println (try (delete-file "/nonexistent-dir-protoclj/x")
              (catch IOException e (ex-message e))))
