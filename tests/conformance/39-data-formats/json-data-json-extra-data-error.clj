;; EXPECT-ERROR: found extra data after the value
;; clojure.data.json's read-str rejects data after the value (D57).
(clojure.data.json/read-str "[1] [2]")
