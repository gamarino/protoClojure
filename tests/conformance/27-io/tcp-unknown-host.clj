;; EXPECT: :unknown-host
;; A host name that does not resolve throws UnknownHostException.
(println (try (tcp-connect "no-such-host.invalid" 80 {:timeout 5000})
              (catch UnknownHostException e (:type (ex-data e)))))
