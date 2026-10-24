;; EXPECT-ERROR: No dispatch macro for: (
;; #(...) is code; EDN has no such dispatch, Clojure's EdnReader message.
(clojure.edn/read-string "#(inc %)")
