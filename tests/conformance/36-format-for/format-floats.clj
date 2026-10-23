;; EXPECT: 3.14|   2.500|1.234568e+04|1.230e-04|1.00000|1,234,567.89|-000003.50|NaN|Infinity
;; format %f %e %g with width, precision and flags.
(println (format "%.2f|%8.3f|%e|%.3e|%g|%,.2f|%010.2f|%f|%f"
                 3.14159 2.5 12345.678 0.000123 1.0 1234567.891 -3.5 ##NaN ##Inf))
