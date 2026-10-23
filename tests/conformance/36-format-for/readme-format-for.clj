;; EXPECT: apple   3 x 0.50 =  1.50|pear    2 x 0.75 =  1.50|(1 9 25)
;; README.md, "A flavour of the language": printf and for (the printed lines
;; are joined here with %s instead of %n, so the runner checks them all).
(doseq [[item qty price] [["apple" 3 0.5] ["pear" 2 0.75]]]
  (printf "%-6s %2d x %.2f = %5.2f%s" item qty price (* qty price) "|"))
(println (for [x (range 10) :when (odd? x) :let [y (* x x)] :while (< y 30)] y))
