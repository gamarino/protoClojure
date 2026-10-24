;; EXPECT: [1 1 1 4 A é 😀 true true 5]
;; String literal escapes, as Clojure's reader reads them: \uXXXX (a
;; surrogate pair is one character), octal up to \377, \b and \f. `count`
;; counts characters, so the pair counts 1 (JVM Clojure: 2 UTF-16 units, D17).
(println [(count "\u0041") (count "\u00e9") (count "\uD83D\uDE00") (count "a\u0000bc")
          "\101" "\u00e9" "\uD83D\uDE00" (= "\b" "\010") (= "\f\t" "\014\11")
          (count "\b\f\r\n\t")])
