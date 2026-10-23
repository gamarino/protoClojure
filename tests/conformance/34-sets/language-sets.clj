;; EXPECT: true true true :a :none true true true
;; LANGUAGE.md §4.4: sets.
(println (= (conj #{:a :b} :c) #{:a :b :c})
         (= (disj #{:a :b :c} :a) #{:b :c})
         (contains? #{:a :b} :a)
         (#{:a :b} :a)
         (get #{:a} :z :none)
         (= (set [1 1 2]) #{1 2})
         (= (into #{} [3 3]) #{3})
         (= (union #{1} #{2}) #{1 2}))
