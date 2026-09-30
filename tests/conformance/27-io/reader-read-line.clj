;; EXPECT: first/second/nil
;; read-line on a reader answers each line without its end, then nil.
(def dir (trim (:out (sh "mktemp" "-d"))))
(def f (str dir "/l.txt"))
(spit f "first\r\nsecond\n")
(def r (reader f))
(def a (read-line r))
(def b (read-line r))
(def c (read-line r))
(close r)
(sh "rm" "-rf" dir)
(println (str a "/" b "/" (if (nil? c) "nil" c)))
