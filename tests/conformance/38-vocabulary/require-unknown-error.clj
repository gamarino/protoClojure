;; EXPECT-ERROR: FileNotFoundException: Could not locate my/lib__init.class
;; A namespace protoClojure does not provide cannot be required (D52).
(require '[my.lib :as m])
