;; EXPECT-ERROR: Invalid 'doseq' keyword :until
;; doseq rejects an unknown modifier when it is expanded.
(doseq [x [1] :until true] (println x))
