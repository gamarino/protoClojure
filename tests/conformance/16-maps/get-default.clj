;; EXPECT: 2 nil :default
(println (get {:a 1 :b 2} :b)
         (get {:a 1 :b 2} :c)
         (get {:a 1 :b 2} :c :default))
