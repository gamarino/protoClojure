;; EXPECT: served body | FileNotFoundException
;; slurp of an http URL fetches it; a 404 throws FileNotFoundException, as java.net.URL does.
(defn handler [req]
  (if (= (:uri req) "/doc") {:status 200 :body "served body"} {:status 404 :body "no"}))
(def s (run-server handler {:port 0}))
(def base (str "http://127.0.0.1:" (server-port s)))
(def text (slurp (str base "/doc")))
(def missing (try (slurp (str base "/nothing")) (catch FileNotFoundException e "FileNotFoundException")))
(stop-server s)
(println text "|" missing)
