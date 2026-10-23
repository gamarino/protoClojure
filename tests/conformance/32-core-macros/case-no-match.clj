;; EXPECT: IllegalArgumentException No matching clause: 7
;; case without a default and no matching constant throws
;; IllegalArgumentException, as in Clojure.
(println (try (case 7 1 :one) (catch IllegalArgumentException e (str "IllegalArgumentException " (ex-message e)))))
