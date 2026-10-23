;; EXPECT: :small :big :none 11
;; condp applies a binary predicate to each test and the expression; :>>
;; passes the predicate's result to a function.
(defn size [n] (condp > n 10 :small 100 :big :none))
(println (size 5) (size 50) (size 500)
         (condp (fn [k m] (get m k)) {:a 10} :b :no :a :>> inc))
