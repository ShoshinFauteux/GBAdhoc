The in-process return rig (docs/INPROC-RETURN.md).  Run inside WSL, PPSSPP on Xvfb :142 in a
private namespace per run (run142.sh, a copy of builds/splash-out/wsl's run136.sh):

  bash tools/inproc/batch_x20.sh <harness eboot dir> <out dir>    # 20 returns + fresh references
  python tools/inproc/x20_analyse.py <out dir>                   # census + per-session identity
  bash tools/inproc/batch_misc.sh <harness eboot dir> <out dir>  # kill switch, wireless refusal,
                                                                 # menu quit, light/GBC, fade-in
Fixtures: ~/drprof/fx (aw2/emerald/hns + states) and ~/inproc-fx (red.gb, crystal.gbc and
their .st0, made with `state_save_at = 600`).

  bash tools/inproc/batch_me_sleep.sh <harness eboot dir> [arms]  # the return keeps the ME module (issue 28):
                                                                 # sleep from the list, wedged engine, the mutant
