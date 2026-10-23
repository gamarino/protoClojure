;; EXPECT: PatternSyntaxException true
;; re-pattern of an invalid pattern raises PatternSyntaxException, an
;; IllegalArgumentException as in Java.
(println (try (re-pattern "a[") (catch PatternSyntaxException e "PatternSyntaxException"))
         (try (re-pattern "(?<=a)b") false (catch IllegalArgumentException e true)))
