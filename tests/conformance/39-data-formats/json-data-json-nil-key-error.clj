;; EXPECT-ERROR: JSON object properties may not be nil
(clojure.data.json/write-str {nil 1})
