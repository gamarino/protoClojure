;; EXPECT: 201 application/json {"name": "ñandú", "tags": [1, 2]}
;; A POST body and its headers reach the handler; header names are lower case.
(defn handler [req]
  {:status 201
   :headers {"content-type" (get (:headers req) "content-type")}
   :body (:body req)})
(def s (run-server handler {:port 0}))
(def r (http-post (str "http://127.0.0.1:" (server-port s) "/items")
                  {:headers {"Content-Type" "application/json"}
                   :body "{\"name\": \"ñandú\", \"tags\": [1, 2]}"}))
(stop-server s)
(println (:status r) (get (:headers r) "content-type") (:body r))
