;; EXPECT: :one :two-or-three :sym :str :vec :default
;; case compares against unevaluated constants; a list groups alternatives;
;; the last odd form is the default.
(defn f [x]
  (case x
    1 :one
    (2 3) :two-or-three
    foo :sym
    "s" :str
    [1 2] :vec
    :default))
(println (f 1) (f 3) (f 'foo) (f "s") (f [1 2]) (f 99))
