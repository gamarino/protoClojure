;; EXPECT-ERROR: unpaired surrogate in a \u escape
;; A malformed document raises an exception naming the problem and the line
;; and column where the input stopped making sense.
(require '[cheshire.core :as json])
(json/parse-string "\"\\uD83D\"")
