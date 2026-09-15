;; EXPECT: true true true false
;; Sequential equality holds past the small inline form of protoCore lists
;; and tuples (300 elements), and a single differing element breaks it.
(defn build [n acc] (if (= n 0) acc (recur (dec n) (cons n acc))))
(def l (build 300 (list)))
(def v (vec l))
(println (= l v)
         (= v (build 300 (list)))
         (= l (rest (cons 0 l)))
         (= l (cons 0 (rest l))))
