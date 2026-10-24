;; EXPECT-ERROR: No such var: str/capitalise
;; A name the aliased namespace does not have is a compile error, as in
;; Clojure.
(require '[clojure.string :as str])
(str/capitalise "x")
