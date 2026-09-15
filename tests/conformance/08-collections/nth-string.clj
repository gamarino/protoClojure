;; EXPECT: b a c true true 1
;; nth on a string returns the character at the index as a one-character
;; string (deviation D3: characters are one-code-point strings). It used to
;; raise "seq op: argument is not a list or vector".
(def s "abc")
(println (nth s 1) (nth s 0) (nth s 2) (string? (nth s 1)) (= (nth s 1) "b")
         (count (nth s 1)))
