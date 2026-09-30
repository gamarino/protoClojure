;; EXPECT-ERROR: Unable to resolve classname: NoSuchException
(try 1 (catch NoSuchException e 2))
