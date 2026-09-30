;; EXPECT: same-origin: secret | cross-origin: none
;; A redirect to the same origin keeps the caller's headers; one to another
;; origin drops them.
(defn echo-auth [req]
  {:status 200 :body (or (get (:headers req) "authorization") "none")})
(def b (run-server echo-auth {:port 0}))
(def b-url (str "http://127.0.0.1:" (server-port b) "/echo"))
(defn front [req]
  (cond (= (:uri req) "/cross") {:status 302 :headers {"location" b-url}}
        (= (:uri req) "/same") {:status 302 :headers {"location" "/echo"}}
        :else (echo-auth req)))
(def a (run-server front {:port 0}))
(def a-base (str "http://127.0.0.1:" (server-port a)))
(def same (http-get (str a-base "/same") {:headers {"authorization" "secret"}}))
(def cross (http-get (str a-base "/cross") {:headers {"authorization" "secret"}}))
(stop-server a)
(stop-server b)
(println "same-origin:" (:body same) "| cross-origin:" (:body cross))
