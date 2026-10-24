;; EXPECT-ERROR: unexpected character ']', expected a value at line 1, column 4
;; A malformed document raises an exception naming the problem and the line
;; and column where the input stopped making sense.
(require '[cheshire.core :as json])
(json/parse-string "[1,]")
