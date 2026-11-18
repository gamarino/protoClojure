;; EXPECT: true false true false false true false true false true true true true true false true false
;; `=` is decided by the values' types: numbers by value across types,
;; strings by content, collections element-wise, and keywords, symbols, fns,
;; atoms, nil and booleans only when identical.
(let [f (fn [] 1)]
  (println (= :a :a) (= :a :b) (= 'a 'a) (= :a "a") (= 'a "a")
           (= inc inc) (= inc dec) (= f f) (= (atom 1) (atom 1))
           (= 1 1.0) (= "ab" (str "a" "b")) (= [1 2] '(1 2)) (= {:a 1} {:a 1})
           (= #{1} #{1}) (= nil false) (= true true) (= :a 'a)))
