;; EXPECT: 5 true 4 1
;; An atom with more than three watches calls each of them once per change,
;; and a watch may run arbitrary code: allocate, swap! other atoms, wait for
;; a future, and remove watches from the atom it watches. Removing watches
;; during a notification affects only later changes.
(def a (atom 0))
(def hits (atom 0))
(def ok (atom true))
(defn build [n acc] (if (= n 0) acc (recur (dec n) (cons n acc))))
(defn busy [k r o n]
  (swap! hits inc)
  (if (= (count (build 2000 (list))) 2000) nil (reset! ok false))
  (if (= @(future (+ o 1)) n) nil (reset! ok false)))
(add-watch a :w1 busy)
(add-watch a :w2 busy)
(add-watch a :w3 busy)
(add-watch a :w4 busy)
(add-watch a :w5 busy)
(reset! a 1)

(def b (atom 0))
(def b-hits (atom 0))
(defn counter [k r o n] (swap! b-hits inc))
(add-watch b :c1 counter)
(add-watch b :c2 counter)
(add-watch b :c3 counter)
(add-watch b :c4 counter)
(add-watch b :killer (fn [k r o n]
                       (remove-watch r :c1) (remove-watch r :c2)
                       (remove-watch r :c3) (remove-watch r :c4)))
(reset! b 1)
(def after-first @b-hits)
(add-watch b :c5 counter)
(reset! b 2)
(println @hits @ok after-first (- @b-hits after-first))
