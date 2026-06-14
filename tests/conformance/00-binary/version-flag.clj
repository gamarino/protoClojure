;; EXPECT: 42
;; v0.0.x sanity: a bare integer top-level form evaluates and its value
;; reaches stdout via the explicit println. Promoted from the earlier
;; EXPECT-ERROR placeholder now that the file runner is wired.
(println 42)
