;; EXPECT-ERROR: No reader function for tag inst
;; There are no tagged-literal readers (#inst, #uuid): Clojure's error for
;; an unknown tag.
(clojure.edn/read-string "#inst \"2026-10-24T00:00:00Z\"")
