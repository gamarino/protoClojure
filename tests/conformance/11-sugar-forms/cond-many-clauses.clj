;; EXPECT: :k0 :k37 :k59 :none
;; A cond whose clauses span more than 255 bytecode instructions: jump
;; offsets are not limited to one byte.
(defn pick [x]
  (cond
    (= x 0) :k0
    (= x 1) :k1
    (= x 2) :k2
    (= x 3) :k3
    (= x 4) :k4
    (= x 5) :k5
    (= x 6) :k6
    (= x 7) :k7
    (= x 8) :k8
    (= x 9) :k9
    (= x 10) :k10
    (= x 11) :k11
    (= x 12) :k12
    (= x 13) :k13
    (= x 14) :k14
    (= x 15) :k15
    (= x 16) :k16
    (= x 17) :k17
    (= x 18) :k18
    (= x 19) :k19
    (= x 20) :k20
    (= x 21) :k21
    (= x 22) :k22
    (= x 23) :k23
    (= x 24) :k24
    (= x 25) :k25
    (= x 26) :k26
    (= x 27) :k27
    (= x 28) :k28
    (= x 29) :k29
    (= x 30) :k30
    (= x 31) :k31
    (= x 32) :k32
    (= x 33) :k33
    (= x 34) :k34
    (= x 35) :k35
    (= x 36) :k36
    (= x 37) :k37
    (= x 38) :k38
    (= x 39) :k39
    (= x 40) :k40
    (= x 41) :k41
    (= x 42) :k42
    (= x 43) :k43
    (= x 44) :k44
    (= x 45) :k45
    (= x 46) :k46
    (= x 47) :k47
    (= x 48) :k48
    (= x 49) :k49
    (= x 50) :k50
    (= x 51) :k51
    (= x 52) :k52
    (= x 53) :k53
    (= x 54) :k54
    (= x 55) :k55
    (= x 56) :k56
    (= x 57) :k57
    (= x 58) :k58
    (= x 59) :k59
    :else :none))
(println (pick 0) (pick 37) (pick 59) (pick 60))
