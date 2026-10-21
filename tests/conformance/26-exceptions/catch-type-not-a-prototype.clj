;; EXPECT: :not-a-type
;; A catch type that resolves to a value that is not a prototype object is an
;; IllegalArgumentException when an exception reaches the clause.
(def not-a-type 42)
(println (try (try (/ 1 0) (catch not-a-type e :wrong))
              (catch IllegalArgumentException e :not-a-type)))
