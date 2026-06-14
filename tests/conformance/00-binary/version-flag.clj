;; EXPECT-ERROR: not yet implemented
;; v0.0.x sanity: the binary launches and rejects an unknown .clj file
;; politely (since we have neither a reader nor a file runner yet). When
;; Week 1 lands the file runner, this fixture will be promoted to a real
;; EXPECT directive that runs the body.
(this would be invalid Clojure but the runner never reaches it in v0.0.x)
