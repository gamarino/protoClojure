;; EXPECT-ERROR: No such var: json/parse
;; A name the namespace does not have is a compile error, as in Clojure.
(require '[cheshire.core :as json])
(json/parse "{}")
