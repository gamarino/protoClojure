;; EXPECT: #<file a/b> a/b inc expects a number, got a file
;; A file prints as a tagged form, renders as its path under str, and error
;; messages name its type.
(def f (file "a" "b"))
(println f (str f) (try (inc f) (catch ClassCastException e (ex-message e))))
