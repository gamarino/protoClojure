;; EXPECT-ERROR: compile error: Syntax error macroexpanding bad: ArithmeticException: Divide by zero
;; A macro that raises fails the compilation of the form that calls it, and
;; the error names the macro.
(defmacro bad [] (/ 1 0))
(defn f [] (bad))
