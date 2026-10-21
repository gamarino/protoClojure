;; EXPECT: 1
;; A catch type is resolved only when an exception reaches its clause, so a
;; try whose body completes never looks it up (CONTRA JVM Clojure, which
;; rejects the unknown class at compile time; DESIGN.md §4.1).
(println (try 1 (catch NoSuchException e 2)))
