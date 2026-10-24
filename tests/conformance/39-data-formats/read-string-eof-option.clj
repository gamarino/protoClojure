;; EXPECT: [:none 1]
;; (read-string {:eof v} s) answers v for a string with no form.
(println [(read-string {:eof :none} " ; a comment only") (read-string {:eof :none} "1")])
