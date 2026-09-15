;; EXPECT: 42 1 nil :none
;; (:k m not-found) returns not-found only when the key is absent; a key
;; mapped to nil returns nil.
(println (:a {} 42)
         (:a {:a 1} 42)
         (:a {:a nil} 42)
         (:missing {:a 1} :none))
