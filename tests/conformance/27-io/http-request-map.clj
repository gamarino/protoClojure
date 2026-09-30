;; EXPECT: :put /r nil 127.0.0.1 HTTP/1.1 :http true true
;; The request map carries Ring's keys.
(def seen (atom nil))
(defn handler [req] (reset! seen req) {:status 204})
(def s (run-server handler {:port 0}))
(def r (http-request {:method :put :uri (str "http://127.0.0.1:" (server-port s) "/r") :body "b"}))
(stop-server s)
(def q @seen)
(println (:request-method q) (:uri q) (if (nil? (:query-string q)) "nil" "?")
         (:remote-addr q) (:protocol q) (:scheme q)
         (= (:server-port q) (server-port s))
         (starts-with? (get (:headers q) "host") "127.0.0.1:"))
