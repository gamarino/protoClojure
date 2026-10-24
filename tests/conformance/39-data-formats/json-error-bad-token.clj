;; EXPECT-ERROR: unrecognized token 'tru', expected a value
;; A malformed document raises an exception naming the problem and the line
;; and column where the input stopped making sense.
(require '[cheshire.core :as json])
(json/parse-string "[tru]")
