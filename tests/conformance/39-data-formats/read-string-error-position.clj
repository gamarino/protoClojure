;; EXPECT-ERROR: Map literal must contain an even number of forms (line 2, column 1)
;; A read error names the reader's message and where in the string it is.
(read-string "\n{:a 1 :b}")
