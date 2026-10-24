;; EXPECT: true true 120 (3 2 1)
;; letfn: mutually recursive local functions (D53: each name calls its
;; definition through an atom).
(letfn [(ev? [n] (if (= n 0) true (od? (dec n))))
        (od? [n] (if (= n 0) false (ev? (dec n))))
        (fact ([n] (fact n 1)) ([n acc] (if (<= n 1) acc (fact (dec n) (* acc n)))))]
  (println (ev? 10) (od? 7) (fact 5) (letfn [(down [n] (when (pos? n) (cons n (down (dec n)))))] (down 3))))
