;; EXPECT: {:a 1, :b 2} {:a 2} {:a {:b 11}} {:a {:b 1}} 2 :nf {:a 1, :b 2} {:a 3, :b 3} {:a 1} nil [:x 2] [1 2 3]
;; zipmap, update, update-in, assoc-in, get-in, merge, merge-with,
;; select-keys; assoc on nil and on a vector.
(println (zipmap [:a :b] [1 2]) (update {:a 1} :a inc) (update-in {:a {:b 1}} [:a :b] + 10)
         (assoc-in {} [:a :b] 1) (get-in {:a {:b 2}} [:a :b]) (get-in {:a 1} [:z] :nf)
         (merge {:a 1} {:b 2} nil) (merge-with + {:a 1} {:a 2 :b 3}) (select-keys {:a 1 :b 2} [:a :c])
         (merge) (assoc [1 2] 0 :x) (assoc [1 2] 2 3))
