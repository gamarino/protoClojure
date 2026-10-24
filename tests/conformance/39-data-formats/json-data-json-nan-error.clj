;; EXPECT-ERROR: cannot write Double NaN
(clojure.data.json/write-str ##NaN)
