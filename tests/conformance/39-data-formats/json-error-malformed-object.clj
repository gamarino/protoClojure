;; EXPECT-ERROR: expected ':' after an object key, found character '1' at line 1, column 6
;; A malformed document raises an exception naming the problem and the line
;; and column where the input stopped making sense.
(require '[cheshire.core :as json])
(json/parse-string "{\"a\" 1}")
