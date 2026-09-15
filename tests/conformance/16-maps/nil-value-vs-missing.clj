;; EXPECT: nil :default true false nil nil
;; A key mapped to nil is present: get returns nil, not the default.
(def m {:a nil})
(println (get m :a :default) (get m :b :default)
         (contains? m :a) (contains? m :b)
         (:a m :d) (m :a :d))
