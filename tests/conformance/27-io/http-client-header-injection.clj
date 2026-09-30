;; EXPECT: IllegalArgumentException :invalid-argument
;; The client refuses a header value with a line break before sending anything.
(println (try (http-get "http://127.0.0.1:9/" {:headers {"x-a" "1\r\nhost: evil"}})
              (catch IllegalArgumentException e (str "IllegalArgumentException " (:type (ex-data e))))))
