;; EXPECT: 2.3 2.67 1 2 10.00 1.0e+01 1e+01
;; %f and %e round half up, as Java's Formatter does (C's printf rounds an
;; exact tie to even); 2.675 is just below its decimal, so it rounds down.
(println (format "%.1f %.2f %.0f %.0f %.2f %.1e %.0e" 2.25 2.675 0.5 1.5 9.999 9.96 9.5))
