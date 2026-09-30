;; EXPECT: true
;; *command-line-args* is nil when the script has no arguments.
(println (nil? *command-line-args*))
