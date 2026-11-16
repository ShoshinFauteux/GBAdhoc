tools/layout/rehearsal -- synthetic cases for tools/layout/repin_rehearsal.sh
(code-health C3-6; docs/LAYOUT-PINNING.md "Rehearsal and what a change now has
to do").  Each patch reproduces the size effect of a past re-pin merge whose
own hunks no longer replay on the pinned tree.  Never applied to a branch:

  repin_rehearsal.sh prepare BASE OUT syn_b4=@tools/layout/rehearsal/b4_hot.patch ...

  b4_hot.patch        the SMC profile back in the release core (B-4's hot rows)
  hw1r2_fe_head.patch +528 B in main_psp.o, before pace_frame (HW-1 r2)
  shots_fe.patch      +2 KiB in video_psp.o, +1 KiB and ~600 B of strings in
                      fe_png.o (the screenshots merge)
