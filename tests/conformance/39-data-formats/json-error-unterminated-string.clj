;; EXPECT-ERROR: unexpected end of input inside a string
;; A malformed document raises an exception naming the problem and the line
;; and column where the input stopped making sense.
(require '[cheshire.core :as json])
(json/parse-string "[\"abc")
