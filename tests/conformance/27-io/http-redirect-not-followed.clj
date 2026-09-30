;; EXPECT: 302 /elsewhere
;; :follow-redirects false answers the redirect itself.
(defn handler [req] {:status 302 :headers {"location" "/elsewhere"}})
(def s (run-server handler {:port 0}))
(def r (http-get (str "http://127.0.0.1:" (server-port s) "/") {:follow-redirects false}))
(stop-server s)
(println (:status r) (get (:headers r) "location"))
