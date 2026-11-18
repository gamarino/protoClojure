;; EXPECT: [No such member: foo (an integer) No such member: foo (a string) No such member: foo (a vector) No such member: foo (an atom) No such member: foo (nil)]
;; `a/b` reads member `b` of the value of `a` through protoCore's attribute
;; lookup, which answers every kind of value; a missing member is a
;; RuntimeException naming the value's type.
(def n 42)
(def s "s")
(def v [1])
(def a (atom 1))
(def z nil)
(println [(try n/foo (catch RuntimeException e (ex-message e)))
          (try s/foo (catch RuntimeException e (ex-message e)))
          (try v/foo (catch RuntimeException e (ex-message e)))
          (try a/foo (catch RuntimeException e (ex-message e)))
          (try z/foo (catch RuntimeException e (ex-message e)))])
