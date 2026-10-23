;; EXPECT: 1 2 3
;; A macro is expanded when a form is compiled. Redefining it changes the
;; forms compiled afterwards; a function compiled before keeps the expansion
;; it got. A `def` of the same name replaces the macro with a value.
(defmacro version [] 1)
(defn old-version [] (version))
(defmacro version [] 2)
(def version 3)
(println (old-version) (eval '(do (defmacro v2 [] 2) (v2))) version)
