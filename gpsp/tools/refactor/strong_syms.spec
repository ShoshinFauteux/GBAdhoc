# strong_syms.spec -- T6 / gate SS (PLAN §5).  One entry per line:
#   name  kind  flavours(all|a,b)  owner-object  weak-stub-objects(-|a.o,b.o)
# hook: overrides a weak no-op in the core; the stub object must still be in the link.
# abi:  a main_psp.c global another TU links against (x1 H5, re-verified in x1's
#       verification); `strong_syms.py golden` adds every other derived one per flavour.

# weak-override hooks (x1 H6; weak stubs rfu.c:96, 113, 351, 403, 440 and main.c:935)
gpsp_visible_done_hook     hook  all  main_psp.o  main.o      # main_psp.c:7541
gpsp_rfu_activated_hook    hook  all  main_psp.o  rfu.o       # main_psp.c:7374
gpsp_rfu_link_down_hook    hook  all  main_psp.o  rfu.o       # main_psp.c:7413
gpsp_rfu_trace_hook        hook  all  main_psp.o  rfu.o       # main_psp.c:7612
gpsp_rfu_now_us            hook  all  main_psp.o  rfu.o       # main_psp.c:7180
gpsp_rfu_flight_dump_hook  hook  all  main_psp.o  rfu.o       # main_psp.c:7185

# exported ABI (x1 H5: ff_psp.c/ff_psp.h, ui_psp.c, ui_share.inc, online_psp.c, usb_handoff.c)
g_ff_uncapped   abi  all  main_psp.o
g_ff_mult       abi  all  main_psp.o
in_rate         abi  all  main_psp.o
g_audio_step    abi  all  main_psp.o
g_net_up        abi  all  main_psp.o
g_net_is_host   abi  all  main_psp.o
g_running       abi  all  main_psp.o
g_dir_base      abi  all  main_psp.o
# harness flavours only (main_psp.c:394 under GPSP_PERF_RIG; callers mgift_net.c:117-118,
# online_psp.c:40-41).  A release has neither the function nor its call sites.
gpsp_net_wait_beat  abi  lab,harness  main_psp.o
