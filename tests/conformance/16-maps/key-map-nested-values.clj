;; EXPECT: :x :x nil :deep
;; A map used as a key is matched by its entries, whatever the insertion
;; order, with its values compared by the same key rules: a list value finds
;; a vector value, but 1.0 does not find 1.
(println (get {{:a [1 2]} :x} {:a (list 1 2)})
         (get {{:a 1 :b 2 :c 3 :d 4 :e 5} :x} (assoc {:e 5 :d 4} :c 3 :b 2 :a 1))
         (get {{:a 1} :x} {:a 1.0})
         (get {{:k {"a-long-string-key" [1.5]}} :deep}
              {:k {(str "a-long-" "string-key") (list (/ 3.0 2))}}))
