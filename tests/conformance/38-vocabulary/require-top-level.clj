;; EXPECT: 1+2 #<file a/b> {:exit 0, :out ok, :err }
;; A plain top-level require records its alias for the forms after it;
;; :refer names are globals already; clojure.java.io and clojure.java.shell
;; name the I/O built-ins.
(require '[clojure.string :as s :refer [join]])
(require '[clojure.java.io :as io] '[clojure.java.shell :refer [sh]])
(println (s/join "+" [1 2]) (io/file "a" "b") (sh "printf" "ok"))
