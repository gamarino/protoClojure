;; EXPECT: false false true x A % | b a a | 5
;; %b, %c (a one-character string or a code point), %%, %n, argument
;; indexes %2$s and %<s.
(println (str (format "%b %b %b %c %c %%" nil false 0 "x" 65) " | "
              (format "%2$s %1$s %<s" "a" "b") " | "
              (count (format "a%nb%nc"))))
