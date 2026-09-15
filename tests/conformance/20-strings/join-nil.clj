;; EXPECT: a,,b|xy|
;; join renders a nil element as the empty string, as str does.
(println (str (join "," ["a" nil "b"]) "|" (join (list "x" nil "y")) "|"
              (join "," (list nil))))
