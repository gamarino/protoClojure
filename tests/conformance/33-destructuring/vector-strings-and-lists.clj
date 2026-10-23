;; EXPECT: a (b c) 1 (2 3) nil
;; A vector binding form destructures anything nth and seq accept: strings
;; (characters are one-character strings, D3), lists and nil.
(let [[x & xs] "abc"
      [y & ys] '(1 2 3)
      [z & zs] nil]
  (println x xs y ys z))
