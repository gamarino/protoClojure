;; EXPECT: :x :y nil
;; Collection keys are matched element by element at every depth: strings
;; built at run time, doubles computed separately, and lists and vectors
;; mixed; 2.0 does not find 2.
(println (get {[["a-long-string-key"] 1.5] :x} [[(str "a-long-" "string-key")] (/ 3.0 2)])
         (get {[(list 1 [2 (list 3)])] :y} (list [1 (list 2 [3])]))
         (get {[1 [2]] :z} [1 [2.0]]))
