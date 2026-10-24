;; EXPECT-ERROR: unexpected end of input inside an array, expected ',' or ']' at line 1, column 5
;; A malformed document raises an exception naming the problem and the line
;; and column where the input stopped making sense.
(require '[cheshire.core :as json])
(json/parse-string "[1,2")
