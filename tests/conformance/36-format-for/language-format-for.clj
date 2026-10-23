;; EXPECT: "cart has 3 items" "ab    |  3.14|00042|ff" "1,234,567" (1 9 25) ([1 :a] [1 :b] [2 :a] [2 :b]) | x=1 :a 1
;; LANGUAGE.md §2.3 (format, printf) and §6.2 (for, doseq).
(print (pr-str (format "%s has %d items" "cart" 3)
     (format "%-6s|%6.2f|%05d|%x" "ab" 3.14159 42 255)
     (format "%,d" 1234567)
     (for [x (range 10) :when (odd? x) :let [y (* x x)] :while (< y 30)] y)
     (for [x [1 2] y [:a :b]] [x y])) "")
(print "| ")
(printf "%s=%d " "x" 1)
(doseq [[k v] {:a 1} :when v] (println k v))
