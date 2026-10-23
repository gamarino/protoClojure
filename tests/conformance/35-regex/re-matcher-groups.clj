;; EXPECT: [1a 1 a] [1a 1 a] [2b 2 b] nil IllegalStateException No match found
;; re-matcher makes a matcher; each re-find on it answers the next match and
;; re-groups the groups of the last one.
(def m (re-matcher #"(\d)(\w)" "1a 2b"))
(def fresh (re-matcher #"x" "x"))
(println (re-find m) (re-groups m) (re-find m) (re-find m)
         (try (re-groups fresh) (catch IllegalStateException e (str "IllegalStateException " (ex-message e)))))
