;; EXPECT: true true false
;; Maps whose keys are collections compare by value: equal keys built
;; separately hash equally, so = finds them.
(println (= {{:a 1} 1 [1 2] 2} {(list 1 2) 2 {:a 1} 1})
         (= {(list 1) :v} {[1] :v})
         (= {{:a 1} 1} {{:a 2} 1}))
