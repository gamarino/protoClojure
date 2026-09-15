;; EXPECT: :found :found nil
;; Hashing recurses through nested collections, so keys that are only equal
;; deep inside still match.
(println (get {[{:a (list 1 2)}] :found} (list {:a [1 2]}))
         (get {{[1] {:b 2 :c 3}} :found} {(list 1) {:c 3 :b 2}})
         (get {{[1] {:b 2}} :found} {(list 1) {:b 3}}))
