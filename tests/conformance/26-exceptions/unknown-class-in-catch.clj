;; EXPECT-ERROR: Unable to resolve classname: NoSuchException
;; A catch clause that names no built-in class names a type resolved at run
;; time, when an exception reaches the clause (DESIGN.md §4.1): a name that
;; resolves to nothing is an error then, never a silent non-match.
(try (/ 1 0) (catch NoSuchException e 2))
