;; EXPECT: 1 2
;; docs/tutorial/11-macros.md §11.5.1: a macro takes effect for the forms
;; compiled after it; a function compiled earlier keeps its expansion.
(defmacro version [] 1)
(defn old-version [] (version))   ; expanded now: the body is the constant 1
(defmacro version [] 2)
(println (old-version) (version))
