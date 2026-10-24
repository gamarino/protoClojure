;; EXPECT-ERROR: invalid escape '\x' in a string
;; A malformed document raises an exception naming the problem and the line
;; and column where the input stopped making sense.
(require '[cheshire.core :as json])
(json/parse-string "\"\\x\"")
