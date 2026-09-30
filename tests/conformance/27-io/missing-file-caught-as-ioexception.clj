;; EXPECT: IOException caught
;; FileNotFoundException is an IOException.
(println (try (slurp "/nonexistent-dir-protoclj/missing.txt")
              (catch IOException e "IOException caught")))
