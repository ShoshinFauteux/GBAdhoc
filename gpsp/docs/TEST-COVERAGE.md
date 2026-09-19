# What each test actually proves

One table, so a passing suite is never mistaken for coverage it does not have.
The rule this file exists to enforce:

> Before quoting a PASS as evidence for a change, check the change is on a row
> this test actually executes.

Several entries below are limitations the scripts already document in their own
headers. They are collected here once so nobody rediscovers them as defects.

## Desktop and host

| test | proves | does NOT prove |
| --- | --- | --- |
| `tools/e2e/run_video_regress.sh` | The core's software scanline renderer is byte-identical over 1091 frames of a fixed Emerald fixture (intro, title, menu, overworld, party screen). Launches and auto-loads correctly **as of `d2857c1`** | **ME execution** — PPSSPP never runs the second core (`me_init reason=handshake magic=0x00000000`), so `g_me_rend` stays 0 and the oracle always hashes the CPU buffer. Also nothing about suspend/resume, same-frame retirement, the GU blit, or what the PSP LCD shows |
| `tools/test_smc_safety.c` | Modelled SMC gate/invalidation invariants hold, including pre-change behaviour and the known role-change hole, which it pins deliberately | Real Allegrex cache behaviour, emitted-code layout or size, translation-cache pressure, or PSP stability. It is a model, not the dynarec |
| `tools/run_host_tests.py` | The host-compilable suites build and pass (FF config/pipeline, Mystery Gift, ROM loading/selection, video buffers, perf rig, SMC safety) | Anything requiring the PSP toolchain's codegen, the ME, or hardware timing |
| `tools/e2e/run_netsmoke.sh` | Transport connectivity at the title screen: the two sides see each other and the link forms | An actual GBA RFU session. It **intentionally expects `core_tx=0`**, documented in its own header — the core has nothing to send at a title screen. A pass says the transport is alive, not that trading works |
| `tools/e2e/run_handoff_test.sh` | Application-side USB handoff logic under PPSSPP: the request/park/resume chain forms, and does not form when handoff is disabled (it carries a negative control) | Physical USB re-enumeration, PSP FAT cache flush behaviour, or whether the host OS remounts the volume |
| `tools/e2e/run_gu_color_test.sh` | The GE blit path and colour conversion | The renderer upstream of it. `run_video_regress.sh` checks the renderer; these are different bugs |

## Hardware only

| question | instrument | why nothing else will do |
| --- | --- | --- |
| ME renderer output correctness | `builds/opus-perf-harness` `me_mode` 0-vs-1 A/B | PPSSPP cannot execute the ME at all |
| dynarec performance | source-pinned hardware A/B, both consoles | PPSSPP gave false reassurance twice: once −14 µs where hardware showed +600 µs |
| suspend / resume | physical sleep/wake on PSP-1000 and PSP Go | PPSSPP does not suspend |
| RFU trading | two real consoles, host and join roles | netsmoke's title-screen link is not a session |
| frame pacing / presentation | hardware, with fast-forward transition checks | vblank phase and LCD behaviour are not emulated faithfully |

## Standing traps

* **A dropped EVT line is not a difference.** The event ring drops lines when a
  frame runs long. Compare only frames both arms logged, and when reading
  "matches no neighbour", confirm the neighbours were actually logged.
* **`perf_window` gaps invalidate aggregates, not runs.** A run whose windows
  have a gap may still have completed — check `perf_done samples=N expected=N`.
  Aggregates from a truncated window set describe a prefix of the run, weighted
  by whatever was happening then, and must not be quoted as performance. A
  `vhash_only` job is never `scoring` for exactly this reason.
* **PPSSPP wears out.** After roughly 40 launch/kill cycles it stops
  initialising and looks exactly like a broken build. Run a known-good control
  first.
* **A harness ini key that is misspelled fails silently** and takes the default.
  Verify keys against the source that reads them.
