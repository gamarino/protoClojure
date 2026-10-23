;; EXPECT: 42|   42|42   |00042|+42|1,234,567|ff|FF|10|(5)|0xff|ffffffffffffffff|123456789012345678901234567890
;; format %d %x %X %o with flags; a negative long in hex is two's
;; complement, as Java prints a Long; big integers keep every digit.
(println (format "%d|%5d|%-5d|%05d|%+d|%,d|%x|%X|%o|%(d|%#x|%x|%d"
                 42 42 42 42 42 1234567 255 255 8 -5 255 -1 123456789012345678901234567890))
