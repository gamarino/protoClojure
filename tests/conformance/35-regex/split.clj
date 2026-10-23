;; EXPECT: ["a" "b" "" "c"] ["a" "b" "c"] ["" "a" "b"] ["a" "b" "c"] ["a" "b,c"] ["a" "b" "" ""] [""]
;; split with a pattern answers a vector, as clojure.string/split: trailing
;; empty strings are dropped unless the limit is negative; a positive limit
;; caps the pieces; "" splits into characters.
(prn (split "a,b,,c,," #",") (split "a1b2c3" #"\d") (split " a  b" #"\s+") (split "abc" #"")
     (split "a,b,c" #"," 2) (split "a,b,," #"," -1) (split "" #","))
