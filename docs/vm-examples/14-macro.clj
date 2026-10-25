(defmacro unless [test then]
  `(if ~test nil ~then))

(unless false :ran)

(when (pos? 1) :yes)
