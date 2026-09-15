;; EXPECT: true false true false
;; empty? of a map is true exactly when it has no entries; a key mapped to nil
;; is an entry.
(println (empty? {})
         (empty? {:a 1})
         (empty? (dissoc {:a 1} :a))
         (empty? {:a nil}))
