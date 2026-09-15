;; EXPECT: 1 2 3 true 3
;; A string built at run time finds the entry of an equal literal string key:
;; long strings, short ASCII strings and short non-ASCII strings alike.
;; assoc under it replaces the value and adds no entry.
(def m {"a-long-string-key" 1 "ab" 2 "año" 3})
(println (get m (str "a-long-" "string-key"))
         (get m (str "a" "b"))
         (get m (str "a" "ño"))
         (contains? m (subs "xa-long-string-keyx" 1 18))
         (count (assoc m (str "a-long-" "string-key") 9)))
