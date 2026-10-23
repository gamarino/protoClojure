;; EXPECT: a.c #"a\d" x+ true true false abc
;; re-pattern compiles a string (a pattern is itself); a pattern prints as
;; #"..." readably and as its source with str; patterns compare by identity,
;; as in Clojure; a literal is compiled once.
(defn lit [] #"q")
(def p (re-pattern "a.c"))
(println p (pr-str #"a\d") (str #"x+") (= (re-pattern p) p) (identical? (lit) (lit))
         (= #"a" #"a") (re-find p "xabcx"))
