;; EXPECT: ##Inf ##-Inf ##NaN [##Inf ##-Inf ##NaN] Infinity -Infinity NaN
;; The reader accepts ##Inf, ##-Inf and ##NaN, the forms the printer uses, so
;; printed floats read back. They used to be "reserved-for-later token: #".
(println ##Inf ##-Inf ##NaN [##Inf ##-Inf ##NaN] (str ##Inf) (str ##-Inf) (str ##NaN))
