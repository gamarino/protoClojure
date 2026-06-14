;; clojure.string-shaped surface in the global namespace (no `ns` yet).
(println (upper-case "hello"))             ;; HELLO
(println (subs "hello world" 6))           ;; world
(println (replace "a-b-c" "-" "/"))        ;; a/b/c
(println (join ", " (list "a" "b" "c")))   ;; a, b, c
(println (split "alice,bob,carol" ","))    ;; (alice bob carol)
(println (trim "   spaced   "))            ;; spaced
(println (blank? "    "))                  ;; true
(println (starts-with? "filename.clj" "filename"))   ;; true
(println (ends-with?   "filename.clj" ".clj"))       ;; true
(println (index-of "the quick brown fox" "quick"))   ;; 4

;; Pipeline — split, map, trim, join.
(defn normalise [s] (upper-case (trim s)))
(println (join " | " (map normalise (split "  a , bb , ccc  " ","))))
