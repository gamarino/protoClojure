;; EXPECT: IllegalFormatConversionException: d != a float | IllegalFormatConversionException: f != an integer | MissingFormatArgumentException: Format specifier '%s'
;; A conversion that does not fit its argument, and a missing argument,
;; raise IllegalArgumentException with Java's exception names (D46).
(defn msg [f & args] (try (apply format (cons f args)) (catch IllegalArgumentException e (ex-message e))))
(println (str (msg "%d" 1.5) " | " (msg "%.2f" 3) " | " (msg "%s %s" 1)))
