;; EXPECT: 10 nil :yes :no 0 nil false :none
;; when-let / if-let bind the test's value when it is truthy; when-some /
;; if-some when it is not nil (false counts as a value).
(println (when-let [x 5] (* x 2)) (when-let [x nil] :never)
         (if-let [x :v] :yes :no) (if-let [x false] :yes :no)
         (when-some [x 0] x) (when-some [x nil] :never)
         (if-some [x false] x :none) (if-some [x nil] x :none))
