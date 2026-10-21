;; EXPECT: [:negative-radius :geometry-error :unrelated-skipped :degenerate :number-format :arithmetic :legacy legacyArea: negative radius :by-var :as-exception :any-value true true :finally-ran :not-a-type true]
;; protoCore's example geometry module (protoCore examples/exceptions/),
;; imported through Unified Module Discovery and run by protoclj_geometry,
;; which links the module's provider. The module's error types are protoCore
;; prototypes; a catch clause names them as module members and selects by the
;; thrown value's prototype chain.
(def geometry (import-module "example/geometry"))
(def NR geometry/NegativeRadius)
(def log (atom nil))

(defn results []
  [;; The module's own type and its base type catch the error.
   (try (geometry/circleArea -1) (catch geometry/NegativeRadius e :negative-radius))
   (try (geometry/circleArea -1) (catch geometry/GeometryError e :geometry-error))
   ;; An unrelated type -- the module's, or a Clojure class -- does not.
   (try (try (geometry/circleArea -1)
             (catch geometry/DegenerateTriangle e :wrong)
             (catch ArithmeticException e :wrong))
        (catch geometry/NegativeRadius e :unrelated-skipped))
   (try (geometry/triangleArea 1 2 10) (catch geometry/DegenerateTriangle e :degenerate))
   ;; protoCore's InvalidArgument error, built by the error factory.
   (try (geometry/parseRadius "ten") (catch NumberFormatException e :number-format))
   ;; protoCore's DivisionByZero error, raised by Clojure's own `/`.
   (try (/ 1 0) (catch ArithmeticException e :arithmetic))
   ;; A C++ std::runtime_error from the module: Clojure's generic error.
   (try (geometry/legacyArea -1) (catch RuntimeException e :legacy))
   (try (geometry/legacyArea -1) (catch RuntimeException e (ex-message e)))
   ;; A var holding the module's type.
   (try (geometry/circleArea -1) (catch NR e :by-var))
   ;; Exception and Throwable catch a module's error too.
   (try (geometry/circleArea -1) (catch Exception e :as-exception))
   (try (geometry/circleArea -1) (catch Throwable e :any-value))
   ;; ex-message reads the module error's message.
   (starts-with? (ex-message (try (geometry/circleArea -1) (catch Object e e)))
                 "circleArea: the radius")
   ;; A rethrow (no clause matched) keeps the very same value.
   (let [caught (try (try (geometry/circleArea -1)
                          (catch ArithmeticException e :wrong))
                     (catch Object e e))]
     (try (throw caught) (catch geometry/NegativeRadius e (= e caught))))
   ;; finally runs while a module error unwinds.
   (try (try (geometry/circleArea -1) (finally (reset! log :finally-ran)))
        (catch geometry/GeometryError e @log))
   ;; A catch type that is not a prototype is an error, not a silent miss.
   (try (try (geometry/circleArea -1) (catch log e :wrong))
        (catch IllegalArgumentException e :not-a-type))
   (< 3.14 (geometry/circleArea 1) 3.15)])
(println (results))
