;; EXPECT: true false false :x true true
;; Sets are equal when they hold the same elements, in any order; numbers
;; of different types are different elements, as in Clojure; a set is a map
;; key and an element of another set by value; elements match as map keys
;; do, so a vector finds an equal list.
(println (= #{1 2} #{2 1}) (= #{1} #{1.0}) (= #{1} [1])
         (get {#{1 2} :x} #{2 1})
         (contains? #{#{:a}} #{:a})
         (contains? #{[1 2]} '(1 2)))
