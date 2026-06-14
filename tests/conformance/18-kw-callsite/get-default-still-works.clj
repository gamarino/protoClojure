;; EXPECT: :default
;; `(get m k not-found)` takes three positionals; trailing `:c :default`
;; must NOT be interpreted as kwArgs for a non-kw-based primitive.
(println (get {:a 1 :b 2} :c :default))
