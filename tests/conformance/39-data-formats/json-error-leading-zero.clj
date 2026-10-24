;; EXPECT-ERROR: leading zeroes are not allowed
;; A malformed document raises an exception naming the problem and the line
;; and column where the input stopped making sense.
(require '[cheshire.core :as json])
(json/parse-string "01")
