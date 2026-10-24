;; EXPECT: [:io :exception]
;; A cheshire parse error is an IOException (Jackson's JsonParseException);
;; a data.json one an Exception.
(println [(try (cheshire.core/parse-string "{") (catch java.io.IOException e :io))
          (try (clojure.data.json/read-str "{") (catch Exception e :exception))])
