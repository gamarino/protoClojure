;; EXPECT: (1 4 9) 3 6 [1 2 (3 4)] {:a 7} [2 1] nil
;; #(...) is (fn* [args] (...)): % and %1 the first argument, %N the Nth,
;; %& the rest; the arity is the largest index used.
(println (map #(* % %) [1 2 3]) (#(+ %1 %2) 1 2) (#(apply + %&) 1 2 3)
         (#(vector % %2 %&) 1 2 3 4) (#(do [%] {:a %}) 7) (#(vector %2 %1) 1 2)
         (#(identity nil)))
