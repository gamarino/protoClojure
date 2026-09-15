;; EXPECT: 0.3333333333333333 0.6666666666666666 0.30000000000000004 1234567.5 3.14 4.9E-324 1.7976931348623157E308
;; Floats print with the shortest digits that read back as the same double,
;; as JVM Clojure (Double.toString) does; %g kept only six significant digits.
(println (/ 1.0 3) (/ 2.0 3) (+ 0.1 0.2) 1234567.5 3.14 4.9e-324 1.7976931348623157e308)
