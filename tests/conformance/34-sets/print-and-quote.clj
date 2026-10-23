;; EXPECT: #{"a"} #{a} true #{5 6} empty
;; A set prints as #{...} (readably with pr-str); a quoted set is a set of
;; unevaluated forms; a syntax-quote template of a set applies hash-set; the
;; elements of a set literal are evaluated.
(def t `#{a ~(+ 1 2)})
(println (pr-str #{"a"}) '#{a} (= t #{'user/a 3}) (let [x 5] #{x (inc x)}) (if (empty? #{}) "empty" "full"))
