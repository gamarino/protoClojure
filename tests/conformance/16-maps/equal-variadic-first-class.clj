;; EXPECT: true false true :same
;; = on maps also works with more than two arguments, as a first-class
;; function value and inside a function body.
(def eq =)
(defn classify [m] (if (= m {:y 2 :x 1}) :same :different))
(println (= {:a 1 :b 2} {:b 2 :a 1} {:a 1 :b 2})
         (= {:a 1} {:a 1} {:a 2})
         (eq {:a 1 :b 2} {:b 2 :a 1})
         (classify {:x 1 :y 2}))
