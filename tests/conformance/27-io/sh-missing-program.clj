;; EXPECT: :process
;; A program that cannot be run throws IOException with {:type :process}.
(println (try (sh "protoclj-no-such-program")
              (catch IOException e (:type (ex-data e)))))
