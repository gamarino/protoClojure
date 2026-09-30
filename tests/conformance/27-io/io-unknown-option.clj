;; EXPECT: spit: unsupported option :apend
;; A misspelt option is refused rather than ignored.
(println (try (spit "/tmp/protoclj-never-written" "x" :apend true)
              (catch IllegalArgumentException e (ex-message e))))
