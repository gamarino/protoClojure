;; EXPECT: IOException
;; A :dir that does not exist throws IOException, as the JVM does.
(println (try (sh "pwd" :dir "/nonexistent-dir-protoclj")
              (catch IOException e "IOException")))
