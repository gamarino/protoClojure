;; EXPECT: PatternSyntaxException true b
;; re-pattern of an invalid pattern raises PatternSyntaxException, an
;; IllegalArgumentException as in Java. Lookbehind is valid, as in Java.
(println (try (re-pattern "a[") (catch PatternSyntaxException e "PatternSyntaxException"))
         (try (re-pattern "a{2,1}") false (catch IllegalArgumentException e true))
         (re-find (re-pattern "(?<=a)b") "cbab"))
