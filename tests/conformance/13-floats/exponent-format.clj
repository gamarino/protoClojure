;; EXPECT: 1.0E21 1.0E7 9999999.0 100.0 0.001 1.0E-4 1.5E-7 1.0E16 1.2345678901234568E20
;; Plain notation for magnitudes from 0.001 up to 10,000,000 (exclusive), and
;; computerized scientific notation (1.0E21, 1.5E-7) outside that range, as
;; JVM Clojure prints doubles.
(println 1e21 1e7 9999999.0 100.0 0.001 1.0E-4 1.5e-7 1e16 123456789012345680000.0)
