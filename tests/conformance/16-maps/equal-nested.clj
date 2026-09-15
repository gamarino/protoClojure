;; EXPECT: true true false false
;; Values are compared with = recursively, so nested maps and vectors
;; (including maps inside vectors) compare structurally.
(println (= {:a {:x 1 :y 2} :b [1 2]} {:b [1 2] :a {:y 2 :x 1}})
         (= {:v [{:p 1 :q 2}]} {:v [{:q 2 :p 1}]})
         (= {:a {:x 1}} {:a {:x 2}})
         (= {:v [{:p 1}]} {:v [{:p 1} 2]}))
