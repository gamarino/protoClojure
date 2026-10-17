;; EXPECT: 3
#! `#!` is a line comment wherever it appears, as in Clojure (the reader's
#! CommentReader); an unbalanced ( inside it does not matter.
(println (+ 1 2)) #! a trailing comment
