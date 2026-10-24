;; EXPECT-ERROR: at line 3, column 11
;; A malformed document raises an exception naming the problem and the line
;; and column where the input stopped making sense.
(require '[cheshire.core :as json])
(json/parse-string "{\n  \"a\": [1,\n        2,,]}")
