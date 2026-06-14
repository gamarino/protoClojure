;; EXPECT: localhost 9090 {:port 9090}
(defn config [& {:keys [host port] :or {host "localhost" port 8080} :as opts}]
  (println host port opts))
(config :port 9090)
