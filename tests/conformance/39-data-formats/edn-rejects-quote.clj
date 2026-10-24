;; EXPECT-ERROR: EDN does not support the reader macro '
;; EDN is data: the quote reader macro is code (D56).
(clojure.edn/read-string "'a")
