# strong_syms.spec -- T6 / gate SS (PLAN §5).  One entry per line:
#   name  kind  flavours(all|a,b)  owner-object  weak-stub-objects(-|a.o,b.o)
# hook: overrides a weak no-op in the core; the stub object must still be in the link.
# abi:  a main_psp.c global another TU links against (x1 H5, re-verified in x1's
#       verification); `strong_syms.py golden` adds every other derived one per flavour.
# Line cites are the P0c cut base's (claude/beta5-base 3bc947d0; HALT_HOP moved main.c's weak
# stub 110 lines down from fbe9da63's; PLAN-P-beta5 §3 P0a T6); each names the line that holds the
# row's symbol, and tests/p0a_test_cites.py holds them to that tree (tests/p0a_b5.sh: strict on
# the tree whose psp/main_psp.c is the cut base's, sha256 9032d9543ad5...).  main_psp.c's lines
# move in P4; the anchor is the symbol's definition, which split.py carries unchanged.

# weak-override hooks (x1 H6; weak stubs rfu.c:96, rfu.c:113, rfu.c:351, rfu.c:411, rfu.c:452
# and main.c:1068)
gpsp_visible_done_hook     hook  all  main_psp.o  main.o      # main_psp.c:7638
gpsp_rfu_activated_hook    hook  all  main_psp.o  rfu.o       # main_psp.c:7469
gpsp_rfu_link_down_hook    hook  all  main_psp.o  rfu.o       # main_psp.c:7508
gpsp_rfu_trace_hook        hook  all  main_psp.o  rfu.o       # main_psp.c:7709
gpsp_rfu_now_us            hook  all  main_psp.o  rfu.o       # main_psp.c:7275
gpsp_rfu_flight_dump_hook  hook  all  main_psp.o  rfu.o       # main_psp.c:7280

# exported ABI (x1 H5: ff_psp.c/ff_psp.h, ui_psp.c, ui_share.inc, online_psp.c, usb_handoff.c)
g_ff_uncapped   abi  all  main_psp.o
g_ff_mult       abi  all  main_psp.o
in_rate         abi  all  main_psp.o
g_audio_step    abi  all  main_psp.o
g_net_up        abi  all  main_psp.o
g_net_is_host   abi  all  main_psp.o
g_running       abi  all  main_psp.o
g_dir_base      abi  all  main_psp.o
# harness flavours only (main_psp.c:394 under GPSP_PERF_RIG; callers mgift_net.c:147-148,
# online_psp.c:41-42).  A release has neither the function nor its call sites.
gpsp_net_wait_beat  abi  lab,harness  main_psp.o
