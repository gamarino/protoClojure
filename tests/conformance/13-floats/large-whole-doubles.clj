;; EXPECT: 1.0E300 -1.0E300 9.223372036854776E18 1.0E19 -1.0E19
;; Whole-number doubles beyond the long long range print in scientific
;; notation. The printer cast them to long long first, which is undefined
;; behaviour for a value out of range.
(println 1e300 -1e300 9.223372036854775807E18 1e19 -1e19)
