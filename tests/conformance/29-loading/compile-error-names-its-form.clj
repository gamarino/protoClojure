;; EXPECT-ERROR: compile-error-names-its-form.clj:5:1: compile error: if: expects (if test then) or (if test then else)
;; A script runs form by form; a compile error names the line and column
;; where its top-level form starts.
(println "this line runs first")
(if)
