;; EXPECT-ERROR: expected a string key in an object, found character 'a' at line 1, column 2
;; A malformed document raises an exception naming the problem and the line
;; and column where the input stopped making sense.
(require '[cheshire.core :as json])
(json/parse-string "{a:1}")
