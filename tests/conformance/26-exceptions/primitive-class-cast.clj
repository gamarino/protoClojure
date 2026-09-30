;; EXPECT: + expects a number, got a string | < expects a number, got nil
(println (try (+ 1 "a") (catch ClassCastException e (ex-message e)))
         "|"
         (try (< nil 1) (catch ClassCastException e (ex-message e))))
