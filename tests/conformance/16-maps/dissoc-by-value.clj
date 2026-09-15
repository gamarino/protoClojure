;; EXPECT: {:k 1} {:a 1} {:m 2} true
;; dissoc matches keys by value, as get does: an equal list removes a vector
;; key, the string ":a" removes only the string key, and an equal map removes
;; a map key. A key mapped to nil stays a key after another key is removed.
(println (dissoc {[1 2] :x :k 1} (list 1 2))
         (dissoc {:a 1 ":a" 2} ":a")
         (dissoc {{:p 1 :q 2} :x :m 2} {:q 2 :p 1})
         (contains? (dissoc {:a nil :b nil} :a) :b))
