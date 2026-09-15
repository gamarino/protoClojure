;; EXPECT: #<fn println> #<fn +> #<fn> [#<fn first> #<fn not=>] #<fn map> true
;; A built-in function prints as #<fn NAME>, in the style of a user fn's
;; #<fn>, in str and println alike, and the form is stable.
(println (str println) (str +) (str (fn [] 1)) [first not=] map
         (= (str reduce) (str reduce)))
